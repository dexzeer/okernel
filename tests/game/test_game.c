// Game engine behaviour tests: levels as strings, run for a few seconds, then look at the world.
// build/run: sh tests/game/run.sh
#include "../../src/game/game_internal.h"
#include <stdio.h>
#include <string.h>

static int failures, checks;
static char logbuf[4096];

static void check(int ok, const char* name, const char* detail) {
    checks++;
    if (!ok) { failures++; printf("FAIL %s %s\n", name, detail ? detail : ""); }
}

static void say(void* u, const char* line) { (void)u; strcat(logbuf, line); strcat(logbuf, "\n"); }

static struct game* play(const char* src, double seconds, const char* keys) {
    struct game* g = game_new();
    game_set_log(g, say, 0);
    logbuf[0] = 0;
    char err[200];
    if (game_load(g, src, strlen(src), err, sizeof err)) { printf("level error: %s\n", err); game_free(g); return 0; }
    for (const char* k = keys; *k; k++) game_key(g, *k == 'u' ? GK_UP : *k == 'd' ? GK_DOWN : *k == 'l' ? GK_LEFT : GK_RIGHT, 1);
    for (double t = 0; t < seconds; t += 1.0 / 60) game_update(g, 1.0 / 60);
    return g;
}

static int count_kind(struct game* g, int kind) {
    int n = 0;
    for (int i = 0; i < G_MAX_ENTS; i++) n += g->ent[i].used && g->ent[i].kind == kind;
    return n;
}

int main(void) {
    struct game* g;

    // the player sits on the road and stays alive
    g = play("from dexlib import world\nwhen run do\n world:block(0, 6, 40, 1)\n guy = world:spawn(\"wheelchair\", 0, 4.6)\nendifchain\n", 3, "");
    check(g && g->state == GS_PLAYING && !g->ent[g->player].dead, "rider sits quietly", 0);
    check(g && g->ent[g->player].limbs_lost == 0, "nothing snaps at rest", 0);
    if (g) game_free(g);

    // spikes end a run
    g = play("from dexlib import world\nwhen run do\n world:block(0, 6, 40, 1)\n world:spikes(4, 5.5, 8)\n guy = world:spawn(\"wheelchair\", 0, 4.6)\nendifchain\n", 6, "u");
    check(g && g->state == GS_DEAD, "spikes kill", 0);
    if (g) game_free(g);

    // the flag ends it the other way
    g = play("from dexlib import world\nwhen run do\n world:block(0, 6, 40, 1)\n world:goal(6, 5.5)\n guy = world:spawn(\"wheelchair\", 0, 4.6)\nendifchain\n", 8, "u");
    check(g && g->state == GS_WON, "the flag wins", 0);
    if (g) game_free(g);

    // `child.abort then suicide`: the thing makes a child, waits, aborts it, and dies. Nothing is left.
    g = play("from dexlib import world\n"
             "thing mama is \"ball\" do\n when run do\n  world:ball(me.x, me.y - 1, 0.25)\n  waitfor:0.5s\n  child.abort then suicide\n endifchain\nendifchain\n"
             "when run do\n world:block(0, 6, 40, 1)\n world:make(\"mama\", 10, 4)\nendifchain\n", 0.3, "");
    check(g && count_kind(g, K_BALL) == 2, "mama and her baby exist", 0);
    if (g) {
        for (double t = 0; t < 1.0; t += 1.0 / 60) game_update(g, 1.0 / 60);
        check(count_kind(g, K_BALL) == 0, "child.abort then suicide leaves nothing", 0);
        game_free(g);
    }

    // a barrel explodes when touched and flings the rider (but does not kill with a small blast)
    g = play("from dexlib import world\nfrom dexlib import fx\n"
             "thing barrel is \"barrel\" do\n when me touches \"guy\" do\n  fx:boom(me, 40)\n  suicide\n endifchain\nendifchain\n"
             "when run do\n world:block(0, 6, 60, 1)\n world:make(\"barrel\", 6, 5.0)\n guy = world:spawn(\"wheelchair\", 0, 4.6)\nendifchain\n", 6, "u");
    check(g && count_kind(g, K_BARREL) == 0, "the barrel went off", 0);
    check(g && g->state != GS_WON, "(sanity) still playing or dead", 0);
    if (g) game_free(g);

    // scripts see the world: member reads and writes, fields on things, text joins
    g = play("from dexlib import world\nwhen run do\n c = world:crate(5, 3)\n c.x = 10\n c.score = 4\n c.score += 3\n log:print(\"x=\" + c.x, c.score, c.kind, c.tag)\nendifchain\n", 0.2, "");
    check(strstr(logbuf, "x=10 7 crate crate") != 0, "members read and write", logbuf);
    if (g) game_free(g);

    // errors stay in the script: a wrong call is reported, the level keeps running
    g = play("from dexlib import world\nwhen run do\n world:block(0, 6, 40, 1)\n world:spikes(\"no\")\nendifchain\nwhen run do\n log:print(\"fine\")\nendifchain\n", 0.5, "");
    check(g && strstr(logbuf, "error") && strstr(logbuf, "fine"), "a script error does not stop the level", logbuf);
    if (g) game_free(g);

    // restart reloads the same level
    g = play("from dexlib import world\nwhen run do\n world:block(0, 6, 40, 1)\n guy = world:spawn(\"wheelchair\", 0, 4.6)\nendifchain\n", 3, "u");
    if (g) {
        double x0 = g->world.body[g->ent[g->player].body[PART_TORSO]].p.x;
        game_key_name(g, "r");
        double x1 = g->world.body[g->ent[g->player].body[PART_TORSO]].p.x;
        check(x0 > 2 && x1 < 1 && game_time(g) == 0, "R restarts", 0);
        game_free(g);
    }

    // a level that does not compile is refused with a line number
    {
        struct game* b = game_new();
        char err[200];
        int bad = game_load(b, "when run do\n x = (\nendifchain\n", 28, err, sizeof err);
        check(bad && strstr(err, "line 2"), "bad level is refused", err);
        game_free(b);
    }

    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures != 0;
}
