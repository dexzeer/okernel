        static const char* const CORPUS[] = {
            "from dexlib import basics\nwhen run do\n x = 42\n if x == 42 do log:print(\"a\")\n elseif x == 10 do log:print(\"b\")\n else do log:print(\"c\")\n endifchain\n waitfor:1s\n suicide\nendifchain\n",
            "when run do\n repeat 3 as i do\n log:print(i, math(\"1+2*3\"))\n endifchain\n checkfor ready 1s do log:print(\"late\")\n endifchain\n above(2)\nendifchain\n",
            "thing bomb is \"ball\" do\n when run do\n me.hp = 3\n child.abort then suicide\n endifchain\n when me touches \"guy\" do\n who.hurt(5)\n endifchain\n endifchain\n",
            "when every 2s do\n n += 1\n log:print(\"n\" + n)\nendifchain\nwhen tick do\n x = x + 1 * 2 % 3\nendifchain\nwhen key \"space\" do log:print(\"jump\")\n",
            "when run do\n forever do\n s = \"ab\" + s\n above(loop)\n endifchain\nendifchain\n",
        };
        static const char* const WORDS[] = { "when", "run", "do", "endifchain", "if", "elseif", "else", "x", "=", "==", "(", ")", ",", ":", ".", "1", "2.5", "\"s\"", "then", "waitfor", "above(1)", "checkfor",
                                             "repeat", "as", "forever", "suicide", "stop", "thing", "is", "&", "|", "not", "+", "-", "*", "/", "%", "\n", "log:print", "me", "child", "tick", "every", "touches", "dies", "key", "from", "import", "+=" };
        unsigned seed = 12345;
        int compiled = 0, rounds = 6000;
        for (int round = 0; round < rounds; round++) {
            char src[2048];
            seed = seed * 1103515245u + 12345u;
            strcpy(src, CORPUS[(seed >> 16) % (sizeof CORPUS / sizeof CORPUS[0])]);
            int edits = 1 + (int)((seed >> 8) % 3);
            for (int e = 0; e < edits; e++) {
                seed = seed * 1103515245u + 12345u;
                size_t len = strlen(src);
                size_t at = len ? (seed >> 16) % len : 0;
                seed = seed * 1103515245u + 12345u;
                switch ((seed >> 16) % 4) {
                case 0: memmove(src + at, src + at + 1, len - at); break;                                  // delete a byte
                case 1: { size_t n = 1 + (seed >> 24) % 8; if (at + n > len) n = len - at; memmove(src + at, src + at + n, len - at - n + 1); break; }   // delete a stretch
                case 2: { const char* w = WORDS[(seed >> 8) % (sizeof WORDS / sizeof WORDS[0])]; size_t n = strlen(w); if (len + n + 2 < sizeof src) { memmove(src + at + n + 1, src + at, len - at + 1); memcpy(src + at, w, n); src[at + n] = ' '; } break; }
                default: if (len + 1 < sizeof src) { memmove(src + at + 1, src + at, len - at + 1); src[at] = (char)(32 + (seed >> 8) % 95); } break;   // a random printable byte
                }
            }
            char err[256];
            dm_program* P = dm_compile(src, strlen(src), err, sizeof err);
            if (!P) continue;
            compiled++;
            struct dm_host h;
            memset(&h, 0, sizeof h);
            h.print = say;
            dm_vm* vm = dm_vm_new(P, &h);
            dm_register_basics(vm);
            out[0] = 0;
            if (!dm_link(vm, err, sizeof err)) {
                dm_start(vm);
                dm_fire_key(vm, "space");
                for (int st = 0; st < 40; st++) dm_step(vm, 25);
            }
            dm_vm_free(vm);
            dm_program_free(P);
            if (strlen(out) > sizeof out - 600) out[0] = 0;
        }
        printf("mutation fuzz: %d of %d mutants compiled\n", compiled, rounds);
        check(compiled > rounds / 20, "the fuzzer reaches the VM", 0);
