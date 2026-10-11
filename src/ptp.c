// Touchpad gestures (see ptp.h).
#include "ptp.h"

void ptp_init(struct ptp* p, int xrange, int yrange, int screen_w) {
    uint8_t* z = (uint8_t*)p;
    for (unsigned i = 0; i < sizeof *p; i++) z[i] = 0;
    p->xrange = xrange > 0 ? xrange : 1;
    p->yrange = yrange > 0 ? yrange : 1;
    p->screen_w = screen_w > 0 ? screen_w : 1024;
}

static int iabs(int v) { return v < 0 ? -v : v; }

// pointer acceleration on whole pixels: slow movements stay exact, fast ones gain up to about 2x
static int accel(int v) {
    int a = iabs(v);
    if (a < 3) return v;
    int m = a > 24 ? 24 : a;
    a += a * (m - 2) / 24;
    return v < 0 ? -a : a;
}

static int scaled(int d, int screen_w, int range, int* rem) {
    int tot = d * screen_w + *rem;
    int q = tot / range;
    *rem = tot - q * range;
    return accel(q);
}

static void finish(struct ptp* p, struct ptp_out* o, int button) {
    int b = button ? 1 : 0;
    if (p->pend_stage == 1) { b |= p->pend; p->pend_stage = 2; }
    else if (p->pend_stage == 2) { p->pend_stage = 0; p->pend = 0; }
    o->buttons = b;
    o->changed = o->dx || o->dy || o->wheel || b != p->lastb;
    p->lastb = b;
}

static void frame(struct ptp* p, uint32_t now, const struct hid_contact* c, int nc, int button, struct ptp_out* o) {
    const struct hid_contact* t[HID_MAXFINGER * 2];
    int nt = 0;
    for (int i = 0; i < nc && nt < HID_MAXFINGER * 2; i++) if (c[i].tip) t[nt++] = &c[i];

    if (nt == 0) {
        if (p->touching) {                                           // lifted: was it a tap?
            int small = p->travel < p->xrange / 40;
            if (small && !button && now - p->t_start < 300 && p->max_f <= 2) {
                p->pend = p->max_f == 2 ? 2 : 1;
                p->pend_stage = 1;
            }
        }
        p->touching = 0; p->have_prev = 0; p->pair_prev = 0;
        return;
    }
    if (!p->touching) {
        p->touching = 1; p->t_start = now; p->travel = 0; p->max_f = 0;
        p->have_prev = 0; p->pair_prev = 0; p->scroll_acc = 0; p->remx = p->remy = 0;
        p->prim_id = t[0]->id;
    }
    if (nt > p->max_f) p->max_f = nt;

    if (nt >= 2) {
        p->have_prev = 0;
        int my = (t[0]->y + t[1]->y) / 2;
        if (p->pair_prev) {
            int d = my - p->pair_y;
            p->travel += iabs(d);
            p->scroll_acc += d;
            int step = p->yrange / 30;
            if (step < 1) step = 1;
            int w = p->scroll_acc / step;
            p->scroll_acc -= w * step;
            o->wheel -= w;                                           // fingers up = scroll down (wheel positive = down)
        }
        p->pair_prev = 1; p->pair_y = my;
        return;
    }
    p->pair_prev = 0;
    const struct hid_contact* f = t[0];
    for (int i = 0; i < nt; i++) if (t[i]->id == p->prim_id) f = t[i];
    if (f->id != p->prim_id) { p->prim_id = f->id; p->have_prev = 0; }
    if (p->have_prev) {
        int dx = f->x - p->px, dy = f->y - p->py;
        p->travel += iabs(dx) + iabs(dy);
        o->dx = scaled(dx, p->screen_w, p->xrange, &p->remx);
        o->dy = scaled(dy, p->screen_w, p->xrange, &p->remy);       // the same pixels per unit on both axes
    }
    p->px = f->x; p->py = f->y; p->have_prev = 1;
}

void ptp_report(struct ptp* p, uint32_t now, const struct hid_contact* c, int nc, int count, int button, struct ptp_out* o) {
    o->dx = o->dy = o->wheel = o->buttons = o->changed = 0;
    if (nc > HID_MAXFINGER) nc = HID_MAXFINGER;
    if (p->expect > 0 && now - p->t_frame > 60) { p->expect = 0; p->nbuf = 0; }           // the rest of the frame never came
    if (p->expect > 0 && count == 0) {                                // continuation of a hybrid frame
        for (int i = 0; i < nc && p->nbuf < HID_MAXFINGER * 2; i++) p->buf[p->nbuf++] = c[i];
        p->expect -= nc;
        if (p->expect > 0) { finish(p, o, button); return; }
        p->expect = 0;
        frame(p, now, p->buf, p->nbuf, button, o);
        p->nbuf = 0;
    } else if (count > nc) {                                          // first report of a hybrid frame
        p->nbuf = 0;
        for (int i = 0; i < nc; i++) p->buf[p->nbuf++] = c[i];
        p->expect = count - nc; p->t_frame = now;
        finish(p, o, button);
        return;
    } else {
        p->expect = 0; p->nbuf = 0;
        frame(p, now, c, nc, button, o);
    }
    finish(p, o, button);
}

void ptp_idle(struct ptp* p, uint32_t now, int button, struct ptp_out* o) {
    o->dx = o->dy = o->wheel = o->buttons = o->changed = 0;
    if (p->expect > 0 && now - p->t_frame > 60) { p->expect = 0; p->nbuf = 0; }
    finish(p, o, button);
}
