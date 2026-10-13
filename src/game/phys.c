// 2D rigid body physics. See phys.h for the model. The solver is the classic sequential-impulse scheme, written out here:
//
//   per frame:  N substeps of
//       find contacts  ->  add gravity  ->  warm start  ->  K iterations over contacts and joints  ->  move bodies
//   Substeps (instead of just more iterations) are what keep ragdolls and stacks calm at a 60 Hz frame.
#include "phys.h"
#include "../dmccl/dm_sys.h"

double sqrt(double);

#define BETA_CONTACT 0.2        // how fast overlap is pushed out (fraction per substep)
#define BETA_JOINT   0.25
#define SLOP         0.004      // overlap we tolerate (meters), keeps resting contacts from chattering
#define MAX_PUSH     4.0        // fastest overlap/joint-error correction (m/s)
#define REST_SPEED   1.0        // closing speed below which things do not bounce

// ---- vector helpers -------------------------------------------------------------------------------------------------

static inline v2 V(double x, double y) { v2 r = { x, y }; return r; }
static inline v2 vadd(v2 a, v2 b) { return V(a.x + b.x, a.y + b.y); }
static inline v2 vsub(v2 a, v2 b) { return V(a.x - b.x, a.y - b.y); }
static inline v2 vmul(v2 a, double k) { return V(a.x * k, a.y * k); }
static inline double vdot(v2 a, v2 b) { return a.x * b.x + a.y * b.y; }
static inline double vcross(v2 a, v2 b) { return a.x * b.y - a.y * b.x; }
static inline v2 vcross_sv(double s, v2 v) { return V(-s * v.y, s * v.x); }       // s x v
static inline double vlen(v2 a) { return sqrt(vdot(a, a)); }
static inline v2 vrot(double c, double s, v2 v) { return V(c * v.x - s * v.y, s * v.x + c * v.y); }
static inline v2 vrot_inv(double c, double s, v2 v) { return V(c * v.x + s * v.y, -s * v.x + c * v.y); }

double sin(double);
double cos(double);

static inline v2 vnorm(v2 a) {
    double l = vlen(a);
    return l > 1e-12 ? vmul(a, 1.0 / l) : V(0, 0);
}

v2 ph_world_point(const struct ph_body* b, v2 local) { return vadd(b->p, vrot(b->c, b->s, local)); }

v2 ph_body_velocity_at(const struct ph_body* b, v2 pt) { return vadd(b->v, vcross_sv(b->w, vsub(pt, b->p))); }

void ph_apply_impulse(struct ph_body* b, v2 imp, v2 pt) {
    b->v = vadd(b->v, vmul(imp, b->im));
    b->w += b->iI * vcross(vsub(pt, b->p), imp);
}

// ---- creating bodies ------------------------------------------------------------------------------------------------

void ph_init(struct ph_world* w) {
    memset(w, 0, sizeof *w);
    w->gravity = V(0, 14.0);          // a bit stronger than earth's: things feel snappier on screen
    w->substeps = 3;
    w->iterations = 8;
}

static int new_body(struct ph_world* w) {
    for (int i = 0; i < PH_MAX_BODIES; i++) {
        if (w->body[i].used) continue;
        struct ph_body* b = &w->body[i];
        memset(b, 0, sizeof *b);
        b->used = 1;
        b->ent = -1;
        b->layer = 1;
        b->mask = 0xFFFFFFFFu;
        b->fric = 0.5;
        b->rest = 0.1;
        b->gscale = 1;
        b->c = 1;
        return i;
    }
    return -1;
}

static void finish_body(struct ph_body* b, double mass, double inertia, int dynamic) {
    b->dynamic = (uint8_t)dynamic;
    if (dynamic && mass > 0) { b->m = mass; b->im = 1.0 / mass; b->I = inertia; b->iI = inertia > 0 ? 1.0 / inertia : 0; }
    else { b->m = b->im = b->I = b->iI = 0; }
}

int ph_add_circle(struct ph_world* w, double x, double y, double r, double density, int dynamic) {
    int i = new_body(w);
    if (i < 0) return -1;
    struct ph_body* b = &w->body[i];
    b->shape = SH_CIRCLE;
    b->p = V(x, y);
    b->radius = r;
    double m = density * 3.14159265358979 * r * r;
    finish_body(b, m, 0.5 * m * r * r, dynamic);
    return i;
}

int ph_add_poly(struct ph_world* w, double x, double y, const v2* verts, int n, double density, int dynamic) {
    if (n < 3 || n > PH_MAX_VERTS) return -1;
    int i = new_body(w);
    if (i < 0) return -1;
    struct ph_body* b = &w->body[i];
    b->shape = SH_POLY;
    b->nv = n;
    double area2 = 0;
    for (int k = 0; k < n; k++) area2 += vcross(verts[k], verts[(k + 1) % n]);
    for (int k = 0; k < n; k++) b->vert[k] = area2 >= 0 ? verts[k] : verts[n - 1 - k];     // make the winding positive
    if (area2 < 0) area2 = -area2;
    // centroid, then move the vertices so the body's origin is its centre of mass
    v2 cen = V(0, 0);
    for (int k = 0; k < n; k++) {
        v2 a = b->vert[k], c = b->vert[(k + 1) % n];
        double cr = vcross(a, c);
        cen = vadd(cen, vmul(vadd(a, c), cr));
    }
    cen = vmul(cen, area2 > 1e-12 ? 1.0 / (3.0 * area2) : 0);
    for (int k = 0; k < n; k++) b->vert[k] = vsub(b->vert[k], cen);
    double ipol = 0;
    for (int k = 0; k < n; k++) {
        v2 a = b->vert[k], c = b->vert[(k + 1) % n];
        ipol += vcross(a, c) * (vdot(a, a) + vdot(a, c) + vdot(c, c));
    }
    for (int k = 0; k < n; k++) {
        v2 e = vsub(b->vert[(k + 1) % n], b->vert[k]);
        b->nrm[k] = vnorm(V(e.y, -e.x));
    }
    b->p = vadd(V(x, y), cen);
    finish_body(b, density * area2 * 0.5, density * ipol / 12.0, dynamic);
    return i;
}

int ph_add_box(struct ph_world* w, double x, double y, double hw, double hh, double density, int dynamic) {
    v2 v[4] = { V(-hw, -hh), V(hw, -hh), V(hw, hh), V(-hw, hh) };
    return ph_add_poly(w, x, y, v, 4, density, dynamic);
}

void ph_remove_joint(struct ph_world* w, int j) { if (j >= 0 && j < PH_MAX_JOINTS) w->joint[j].used = 0; }

void ph_remove_body(struct ph_world* w, int body) {
    if (body < 0 || body >= PH_MAX_BODIES) return;
    w->body[body].used = 0;
    for (int j = 0; j < PH_MAX_JOINTS; j++)
        if (w->joint[j].used && (w->joint[j].a == body || w->joint[j].b == body)) w->joint[j].used = 0;
    for (int k = 0; k < PH_MAX_BODIES / 32; k++) {
        w->pair_seen[body][k] = 0;
        w->pair_now[body][k] = 0;
    }
    for (int o = 0; o < PH_MAX_BODIES; o++) {
        w->pair_seen[o][body / 32] &= ~(1u << (body % 32));
        w->pair_now[o][body / 32] &= ~(1u << (body % 32));
    }
}

// ---- joints ---------------------------------------------------------------------------------------------------------

int ph_add_pin_local(struct ph_world* w, int a, v2 la, int b, v2 lb) {
    for (int j = 0; j < PH_MAX_JOINTS; j++) {
        if (w->joint[j].used) continue;
        struct ph_joint* J = &w->joint[j];
        memset(J, 0, sizeof *J);
        J->used = 1;
        J->a = a;
        J->b = b;
        J->la = la;
        J->lb = lb;
        J->ref = w->body[b].a - w->body[a].a;
        return j;
    }
    return -1;
}

int ph_add_pin(struct ph_world* w, int a, int b, v2 pt) {
    struct ph_body *A = &w->body[a], *B = &w->body[b];
    A->c = cos(A->a); A->s = sin(A->a);
    B->c = cos(B->a); B->s = sin(B->a);
    v2 la = vrot_inv(A->c, A->s, vsub(pt, A->p));
    v2 lb = vrot_inv(B->c, B->s, vsub(pt, B->p));
    return ph_add_pin_local(w, a, la, b, lb);
}

void ph_joint_limit(struct ph_world* w, int j, double lo, double hi) {
    if (j < 0) return;
    w->joint[j].limit = 1;
    w->joint[j].lo = lo;
    w->joint[j].hi = hi;
}

void ph_joint_motor(struct ph_world* w, int j, double speed, double max_torque) {
    if (j < 0) return;
    w->joint[j].motor = max_torque > 0;
    w->joint[j].motor_speed = speed;
    w->joint[j].motor_torque = max_torque;
}

// ---- collision: shapes in, contact points out -------------------------------------------------------------------------

struct manifold { v2 n; int np; v2 pt[2]; double depth[2]; int fid[2]; };

static int circle_circle(const struct ph_body* A, const struct ph_body* B, struct manifold* m) {
    v2 d = vsub(B->p, A->p);
    double r = A->radius + B->radius, dist2 = vdot(d, d);
    if (dist2 >= r * r) return 0;
    double dist = sqrt(dist2);
    m->n = dist > 1e-9 ? vmul(d, 1.0 / dist) : V(0, 1);
    m->np = 1;
    m->depth[0] = r - dist;
    m->pt[0] = vadd(A->p, vmul(m->n, A->radius));
    m->fid[0] = 0;
    return 1;
}

// polygon P against circle C; the normal points from P to C
static int poly_circle(const struct ph_body* P, const struct ph_body* C, struct manifold* m) {
    v2 c = vrot_inv(P->c, P->s, vsub(C->p, P->p));          // circle centre in the polygon's frame
    double best = -1e30;
    int face = 0;
    for (int i = 0; i < P->nv; i++) {
        double sep = vdot(P->nrm[i], vsub(c, P->vert[i]));
        if (sep > best) { best = sep; face = i; }
    }
    if (best > C->radius) return 0;
    v2 v1 = P->vert[face], v2_ = P->vert[(face + 1) % P->nv];
    m->np = 1;
    m->fid[0] = face;
    if (best < 1e-9) {                                      // the centre is inside: push out along the nearest face
        m->n = vrot(P->c, P->s, P->nrm[face]);
        m->depth[0] = C->radius - best;
        m->pt[0] = vsub(C->p, vmul(m->n, C->radius));
        return 1;
    }
    v2 closest;
    if (vdot(vsub(c, v1), vsub(v2_, v1)) <= 0) closest = v1;                    // nearest to the first corner
    else if (vdot(vsub(c, v2_), vsub(v1, v2_)) <= 0) closest = v2_;             // nearest to the second corner
    else {                                                                      // nearest to the flat of the face
        m->n = vrot(P->c, P->s, P->nrm[face]);
        m->depth[0] = C->radius - best;
        m->pt[0] = vsub(C->p, vmul(m->n, C->radius));
        return 1;
    }
    v2 d = vsub(c, closest);
    double dist2 = vdot(d, d);
    if (dist2 > C->radius * C->radius) return 0;
    double dist = sqrt(dist2);
    v2 nl = dist > 1e-9 ? vmul(d, 1.0 / dist) : P->nrm[face];
    m->n = vrot(P->c, P->s, nl);
    m->depth[0] = C->radius - dist;
    m->pt[0] = ph_world_point(P, closest);
    return 1;
}

// How far polygon B reaches behind the best face of polygon A (negative = overlap); `edge` = that face.
static double max_separation(const v2* wa, const v2* na, int nva, const v2* wb, int nvb, int* edge) {
    double best = -1e30;
    for (int i = 0; i < nva; i++) {
        double lo = 1e30;
        for (int k = 0; k < nvb; k++) {
            double s = vdot(na[i], vsub(wb[k], wa[i]));
            if (s < lo) lo = s;
        }
        if (lo > best) { best = lo; *edge = i; }
    }
    return best;
}

static int clip_segment(v2 out[2], const v2 in[2], v2 n, double offset) {   // keep the part with dot(n, p) >= offset
    int k = 0;
    double d0 = vdot(n, in[0]) - offset, d1 = vdot(n, in[1]) - offset;
    if (d0 >= 0) out[k++] = in[0];
    if (d1 >= 0) out[k++] = in[1];
    if (d0 * d1 < 0 && k < 2) {
        double t = d0 / (d0 - d1);
        out[k++] = vadd(in[0], vmul(vsub(in[1], in[0]), t));
    }
    return k;
}

static int poly_poly(const struct ph_body* A, const struct ph_body* B, struct manifold* m) {
    v2 wa[PH_MAX_VERTS], na[PH_MAX_VERTS], wb[PH_MAX_VERTS], nb[PH_MAX_VERTS];
    for (int i = 0; i < A->nv; i++) { wa[i] = ph_world_point(A, A->vert[i]); na[i] = vrot(A->c, A->s, A->nrm[i]); }
    for (int i = 0; i < B->nv; i++) { wb[i] = ph_world_point(B, B->vert[i]); nb[i] = vrot(B->c, B->s, B->nrm[i]); }
    int ea = 0, eb = 0;
    double sa = max_separation(wa, na, A->nv, wb, B->nv, &ea);
    if (sa > 0) return 0;
    double sb = max_separation(wb, nb, B->nv, wa, A->nv, &eb);
    if (sb > 0) return 0;
    int a_ref = sb <= sa + 0.0005;                          // prefer A as the reference face unless B clearly is
    const v2 *rw = a_ref ? wa : wb, *rn = a_ref ? na : nb, *iw = a_ref ? wb : wa, *in = a_ref ? nb : na;
    int rnv = a_ref ? A->nv : B->nv, inv = a_ref ? B->nv : A->nv, e = a_ref ? ea : eb;
    v2 refn = rn[e];
    int ie = 0;                                             // the incident face: the one most opposed to the reference normal
    double lowest = 1e30;
    for (int i = 0; i < inv; i++) {
        double d = vdot(refn, in[i]);
        if (d < lowest) { lowest = d; ie = i; }
    }
    v2 seg[2] = { iw[ie], iw[(ie + 1) % inv] }, c1[2], c2[2];
    v2 r1 = rw[e], r2 = rw[(e + 1) % rnv];
    v2 tang = vnorm(vsub(r2, r1));
    if (clip_segment(c1, seg, tang, vdot(tang, r1)) < 2) return 0;
    if (clip_segment(c2, c1, vmul(tang, -1), -vdot(tang, r2)) < 2) return 0;
    m->n = a_ref ? refn : vmul(refn, -1);
    m->np = 0;
    for (int k = 0; k < 2; k++) {
        double sep = vdot(refn, vsub(c2[k], r1));
        if (sep > 0.0005) continue;                         // clipped point floats above the face: no contact
        m->pt[m->np] = c2[k];
        m->depth[m->np] = -sep;
        m->fid[m->np] = (e * 8 + ie) * 2 + k + (a_ref ? 0 : 1000);
        m->np++;
    }
    return m->np > 0;
}

static int collide(const struct ph_body* A, const struct ph_body* B, struct manifold* m) {
    if (A->shape == SH_CIRCLE && B->shape == SH_CIRCLE) return circle_circle(A, B, m);
    if (A->shape == SH_POLY && B->shape == SH_CIRCLE) return poly_circle(A, B, m);
    if (A->shape == SH_CIRCLE && B->shape == SH_POLY) {
        if (!poly_circle(B, A, m)) return 0;
        m->n = vmul(m->n, -1);                              // we were asked for A -> B
        return 1;
    }
    return poly_poly(A, B, m);
}

static double bound(const struct ph_body* b) {
    if (b->shape == SH_CIRCLE) return b->radius;
    double r = 0;
    for (int i = 0; i < b->nv; i++) { double l = vlen(b->vert[i]); if (l > r) r = l; }
    return r;
}

// ---- one substep ----------------------------------------------------------------------------------------------------

static int bit_get(uint32_t (*set)[PH_MAX_BODIES / 32], int a, int b) { return (set[a][b / 32] >> (b % 32)) & 1; }
static void bit_set(uint32_t (*set)[PH_MAX_BODIES / 32], int a, int b) { set[a][b / 32] |= 1u << (b % 32); }

static void emit(struct ph_world* w, struct ph_event e) { if (w->nev < PH_MAX_EVENTS) w->ev[w->nev++] = e; }

static void find_contacts(struct ph_world* w) {
    memcpy(w->prev, w->con, sizeof(struct ph_contact) * (size_t)w->ncon);
    w->nprev = w->ncon;
    w->ncon = 0;
    double radius[PH_MAX_BODIES];
    for (int i = 0; i < PH_MAX_BODIES; i++) if (w->body[i].used) radius[i] = bound(&w->body[i]);
    for (int i = 0; i < PH_MAX_BODIES; i++) {
        struct ph_body* A = &w->body[i];
        if (!A->used) continue;
        for (int j = i + 1; j < PH_MAX_BODIES; j++) {
            struct ph_body* B = &w->body[j];
            if (!B->used || (!A->dynamic && !B->dynamic)) continue;
            if (A->group && A->group == B->group) continue;
            if (!((A->layer & B->mask) && (B->layer & A->mask))) continue;
            v2 d = vsub(B->p, A->p);
            double reach = radius[i] + radius[j];
            if (vdot(d, d) > reach * reach) continue;
            struct manifold m;
            if (!collide(A, B, &m)) continue;
            for (int k = 0; k < m.np && w->ncon < PH_MAX_CONTACTS; k++) {
                struct ph_contact* c = &w->con[w->ncon++];
                memset(c, 0, sizeof *c);
                c->a = i;
                c->b = j;
                c->n = m.n;
                c->pt = m.pt[k];
                c->depth = m.depth[k];
                c->fid = m.fid[k] + (A->sensor || B->sensor ? 100000 : 0);
                c->fric = sqrt(A->fric * B->fric);
                for (int q = 0; q < w->nprev; q++)           // warm start: the same contact one substep ago
                    if (w->prev[q].a == i && w->prev[q].b == j && w->prev[q].fid == c->fid) { c->acc_n = w->prev[q].acc_n; c->acc_t = w->prev[q].acc_t; break; }
            }
        }
    }
}

static void apply(struct ph_body* A, struct ph_body* B, v2 ra, v2 rb, v2 imp) {      // impulse acts on B, minus that on A
    A->v = vsub(A->v, vmul(imp, A->im));
    A->w -= A->iI * vcross(ra, imp);
    B->v = vadd(B->v, vmul(imp, B->im));
    B->w += B->iI * vcross(rb, imp);
}

static void prestep(struct ph_world* w, double h) {
    for (int i = 0; i < w->ncon; i++) {
        struct ph_contact* c = &w->con[i];
        struct ph_body *A = &w->body[c->a], *B = &w->body[c->b];
        c->ra = vsub(c->pt, A->p);
        c->rb = vsub(c->pt, B->p);
        v2 rel = vsub(vadd(B->v, vcross_sv(B->w, c->rb)), vadd(A->v, vcross_sv(A->w, c->ra)));
        double vn = vdot(rel, c->n);
        if (!bit_get(w->pair_seen, c->a, c->b) && !bit_get(w->pair_now, c->a, c->b)) {      // a new touch: tell the game
            struct ph_event e;
            memset(&e, 0, sizeof e);
            e.kind = PE_TOUCH;
            e.a = c->a;
            e.b = c->b;
            e.speed = vn < 0 ? -vn : 0;
            e.pt = c->pt;
            emit(w, e);
        }
        bit_set(w->pair_now, c->a, c->b);
        if (A->sensor || B->sensor) continue;
        double rna = vcross(c->ra, c->n), rnb = vcross(c->rb, c->n);
        double kn = A->im + B->im + A->iI * rna * rna + B->iI * rnb * rnb;
        v2 t = V(c->n.y, -c->n.x);
        double rta = vcross(c->ra, t), rtb = vcross(c->rb, t);
        double kt = A->im + B->im + A->iI * rta * rta + B->iI * rtb * rtb;
        c->mass_n = kn > 0 ? 1.0 / kn : 0;
        c->mass_t = kt > 0 ? 1.0 / kt : 0;
        double over = c->depth - SLOP;
        c->bias = over > 0 ? BETA_CONTACT * over / h : 0;
        if (c->bias > MAX_PUSH) c->bias = MAX_PUSH;             // deep overlaps (a spawn inside a wall) ease out instead of exploding
        double e = A->rest > B->rest ? A->rest : B->rest;
        c->rest_v = (-vn > REST_SPEED) ? -e * vn : 0;
        // warm start
        v2 imp = vadd(vmul(c->n, c->acc_n), vmul(t, c->acc_t));
        apply(A, B, c->ra, c->rb, imp);
    }
}

static void solve_contacts(struct ph_world* w) {
    for (int i = 0; i < w->ncon; i++) {
        struct ph_contact* c = &w->con[i];
        struct ph_body *A = &w->body[c->a], *B = &w->body[c->b];
        if (A->sensor || B->sensor) continue;
        v2 t = V(c->n.y, -c->n.x);
        v2 rel = vsub(vadd(B->v, vcross_sv(B->w, c->rb)), vadd(A->v, vcross_sv(A->w, c->ra)));
        double lt = -vdot(rel, t) * c->mass_t;                      // friction first, limited by what the normal force allows
        double maxf = c->fric * c->acc_n;
        double nt = c->acc_t + lt;
        nt = nt > maxf ? maxf : nt < -maxf ? -maxf : nt;
        lt = nt - c->acc_t;
        c->acc_t = nt;
        apply(A, B, c->ra, c->rb, vmul(t, lt));
        rel = vsub(vadd(B->v, vcross_sv(B->w, c->rb)), vadd(A->v, vcross_sv(A->w, c->ra)));
        double vn = vdot(rel, c->n);
        double want = c->bias > c->rest_v ? c->bias : c->rest_v;
        double ln = (want - vn) * c->mass_n;
        double nn = c->acc_n + ln;
        if (nn < 0) nn = 0;
        ln = nn - c->acc_n;
        c->acc_n = nn;
        apply(A, B, c->ra, c->rb, vmul(c->n, ln));
    }
}

static void solve_joints(struct ph_world* w, double h, int first_iteration) {
    for (int j = 0; j < PH_MAX_JOINTS; j++) {
        struct ph_joint* J = &w->joint[j];
        if (!J->used) continue;
        struct ph_body *A = &w->body[J->a], *B = &w->body[J->b];
        if (!A->used || !B->used) { J->used = 0; continue; }
        if (first_iteration) {
            // warm start with last substep's impulses
            v2 ra = vrot(A->c, A->s, J->la), rb = vrot(B->c, B->s, J->lb);
            apply(A, B, ra, rb, V(J->acc_x, J->acc_y));
            double ang = J->acc_lim + J->acc_motor;
            A->w -= A->iI * ang;
            B->w += B->iI * ang;
        }
        // the pin: both anchors must sit on the same point
        v2 ra = vrot(A->c, A->s, J->la), rb = vrot(B->c, B->s, J->lb);
        v2 err = vsub(vadd(B->p, rb), vadd(A->p, ra));
        v2 rel = vsub(vadd(B->v, vcross_sv(B->w, rb)), vadd(A->v, vcross_sv(A->w, ra)));
        double k11 = A->im + B->im + A->iI * ra.y * ra.y + B->iI * rb.y * rb.y;
        double k12 = -A->iI * ra.x * ra.y - B->iI * rb.x * rb.y;
        double k22 = A->im + B->im + A->iI * ra.x * ra.x + B->iI * rb.x * rb.x;
        double det = k11 * k22 - k12 * k12;
        if (det > 1e-12) {
            v2 fix = vmul(err, BETA_JOINT / h);
            double fl = vlen(fix);
            if (fl > MAX_PUSH) fix = vmul(fix, MAX_PUSH / fl);      // a joint pulled far apart closes at a sane speed
            v2 rhs = V(-(rel.x + fix.x), -(rel.y + fix.y));
            v2 imp = V((k22 * rhs.x - k12 * rhs.y) / det, (-k12 * rhs.x + k11 * rhs.y) / det);
            J->acc_x += imp.x;
            J->acc_y += imp.y;
            apply(A, B, ra, rb, imp);
        }
        double iisum = A->iI + B->iI;
        if (iisum <= 0) continue;
        if (J->motor) {
            double cdot = B->w - A->w - J->motor_speed;
            double l = -cdot / iisum, maxl = J->motor_torque * h;
            double na = J->acc_motor + l;
            na = na > maxl ? maxl : na < -maxl ? -maxl : na;
            l = na - J->acc_motor;
            J->acc_motor = na;
            A->w -= A->iI * l;
            B->w += B->iI * l;
        }
        if (J->limit) {
            double th = B->a - A->a - J->ref;
            double cdot = B->w - A->w;
            if (th <= J->lo + 0.0) {                                  // below the lower stop: push up
                double c = th - J->lo, bias = c > 0 ? c / h : 0.2 * c / h;
                double l = -(cdot + bias) / iisum;
                double nl = J->acc_lim + l;
                if (nl < 0) nl = 0;
                l = nl - J->acc_lim;
                J->acc_lim = nl;
                A->w -= A->iI * l;
                B->w += B->iI * l;
            } else if (th >= J->hi) {                                 // above the upper stop: push down
                double c = J->hi - th, bias = c > 0 ? c / h : 0.2 * c / h;
                double l = -(-cdot + bias) / iisum;
                double nl = J->acc_lim - l;                           // acc_lim is signed: negative pushes down
                if (nl > 0) nl = 0;
                double dl = nl - J->acc_lim;
                J->acc_lim = nl;
                A->w -= A->iI * dl;
                B->w += B->iI * dl;
            }
        }
    }
}

static void check_breaks(struct ph_world* w, double h) {
    for (int j = 0; j < PH_MAX_JOINTS; j++) {
        struct ph_joint* J = &w->joint[j];
        if (!J->used) continue;
        J->last_force = sqrt(J->acc_x * J->acc_x + J->acc_y * J->acc_y) / h;
        if (J->break_force > 0 && J->last_force > J->break_force) {
            struct ph_event e;
            memset(&e, 0, sizeof e);
            e.kind = PE_JOINT_BREAK;
            e.a = J->a;
            e.b = J->b;
            e.impulse = J->last_force * h;
            e.pt = vadd(w->body[J->a].p, vrot(w->body[J->a].c, w->body[J->a].s, J->la));
            emit(w, e);
            J->used = 0;
        }
    }
}

static void substep(struct ph_world* w, double h) {
    for (int i = 0; i < PH_MAX_BODIES; i++) {
        struct ph_body* b = &w->body[i];
        if (!b->used) continue;
        b->c = cos(b->a);
        b->s = sin(b->a);
    }
    find_contacts(w);
    for (int i = 0; i < PH_MAX_BODIES; i++) {                       // forces first, so the solver sees them
        struct ph_body* b = &w->body[i];
        if (!b->used || !b->dynamic) continue;
        b->v = vadd(b->v, vmul(w->gravity, h * b->gscale));
        double dl = 1.0 / (1.0 + h * b->damp_lin), da = 1.0 / (1.0 + h * b->damp_ang);
        b->v = vmul(b->v, dl);
        b->w *= da;
    }
    prestep(w, h);
    for (int it = 0; it < w->iterations; it++) {
        solve_joints(w, h, it == 0);
        solve_contacts(w);
    }
    check_breaks(w, h);
    for (int i = 0; i < PH_MAX_BODIES; i++) {
        struct ph_body* b = &w->body[i];
        if (!b->used || !b->dynamic) continue;
        b->p = vadd(b->p, vmul(b->v, h));
        b->a += b->w * h;
    }
}

void ph_step(struct ph_world* w, double dt) {
    w->nev = 0;
    int n = w->substeps < 1 ? 1 : w->substeps;
    for (int s = 0; s < n; s++) substep(w, dt / n);
    w->time += dt;
    // the touch events learn how hard the hardest contact point pushed
    for (int e = 0; e < w->nev; e++) {
        if (w->ev[e].kind != PE_TOUCH) continue;
        for (int i = 0; i < w->ncon; i++)
            if (w->con[i].a == w->ev[e].a && w->con[i].b == w->ev[e].b && w->con[i].acc_n > w->ev[e].impulse) w->ev[e].impulse = w->con[i].acc_n;
    }
    memcpy(w->pair_seen, w->pair_now, sizeof w->pair_seen);
    memset(w->pair_now, 0, sizeof w->pair_now);
}

// ---- queries ----------------------------------------------------------------------------------------------------------

int ph_body_at(const struct ph_world* w, v2 p) {
    for (int i = PH_MAX_BODIES - 1; i >= 0; i--) {
        const struct ph_body* b = &w->body[i];
        if (!b->used) continue;
        v2 d = vsub(p, b->p);
        if (b->shape == SH_CIRCLE) { if (vdot(d, d) <= b->radius * b->radius) return i; continue; }
        double c = cos(b->a), s = sin(b->a);
        v2 l = V(c * d.x + s * d.y, -s * d.x + c * d.y);
        int inside = 1;
        for (int k = 0; k < b->nv && inside; k++) if (vdot(b->nrm[k], vsub(l, b->vert[k])) > 0) inside = 0;
        if (inside) return i;
    }
    return -1;
}

double ph_kinetic_energy(const struct ph_world* w) {
    double e = 0;
    for (int i = 0; i < PH_MAX_BODIES; i++) {
        const struct ph_body* b = &w->body[i];
        if (b->used && b->dynamic) e += 0.5 * b->m * vdot(b->v, b->v) + 0.5 * b->I * b->w * b->w;
    }
    return e;
}
