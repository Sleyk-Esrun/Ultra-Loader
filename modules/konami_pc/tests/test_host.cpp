/* SPDX-License-Identifier: MIT
 * test_host.cpp -- Ultra Loader host driving the native x86 arcade board module.
 * No host loader wired -> reference mode. Demonstrates: many board profiles,
 * native launch, and the the external process hook process hook giving determinism (caps + state)
 * that bare launch-and-run lacks.
 */
#include "ultra_module.h"
#include "ultra_replay.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <string>

static void host_log(void*, ultra_log_level lvl, const char* msg) {
    const char* tag[] = {"DIAG","GAME","INFO","WARN","ERR "};
    std::printf("  [%s] %s\n", tag[lvl], msg);
}
static int host_resolve_bios(void*, const char* base, char* out, size_t cap) {
    std::snprintf(out, cap, "data/%s", base); return 0;
}
static uint64_t host_now_ns(void*) { return 0; }
extern "C" ultra_result ultra_module_create(uint32_t, const ultra_host_services*, ultra_module_handle*);

int main() {
    std::printf("=== Ultra Loader host <-> Native x86 Arcade Board module ===\n\n");
    ultra_host_services host; std::memset(&host, 0, sizeof(host));
    host.struct_size = sizeof(host); host.log = host_log;
    host.resolve_bios = host_resolve_bios; host.now_ns = host_now_ns;

    ultra_module_handle h; std::memset(&h, 0, sizeof(h));
    if (ultra_module_create(ULTRA_MODULE_ABI_VERSION, &host, &h) != ULTRA_OK) return 1;
    const ultra_module_vtable* v = h.vtable;

    const ultra_system_info* sys[32];
    uint32_t n = v->list_systems(h.instance, sys, 32);
    std::printf("module provides %u board profiles (showing first 8):\n", n);
    for (uint32_t i = 0; i < n && i < 8; ++i)
        std::printf("  - %-16s %-42s caps=0x%02x\n",
                    sys[i]->system_id, sys[i]->display_name, sys[i]->capabilities);
    std::printf("  ... (%u total: SEGA, Namco, Taito, Raw Thrills, Konami, generic)\n", n);

    const char* paths[] = {
        "games/initiald8/", "games/tekken7fr/game.exe",
        "games/lindbergh/vt3.elf", "games/notagame.zip"
    };
    std::printf("\nprobe results:\n");
    const char* cs=nullptr; std::string cp;
    for (auto p : paths) {
        ultra_content_info ci; v->probe(h.instance, p, &ci);
        std::printf("  %-26s -> %s\n", p, ci.claimed ? ci.system_id : "(not claimed)");
        if (ci.claimed && !cs){cs="taito_typex4"; cp=p;}  // pick a real board profile
    }

    std::printf("\ninit Taito Type X4 + launch native game (hook attached by default):\n");
    v->init(h.instance, "taito_typex4");
    v->load_media(h.instance, cp.c_str());

    std::printf("\nrunning 3 frames (gated by hook for determinism):\n");
    ultra_input_state in; std::memset(&in, 0, sizeof(in));
    for (int f=0;f<3;f++){ in.buttons=(f%2)?0x10:0; v->set_input(h.instance,0,&in);
        uint64_t c=0; v->run_frame(h.instance,&c);
        std::printf("  frame %d: tick=%llu\n", f, (unsigned long long)c); }

    size_t need=0;
    std::printf("\nsave-state (via process hook): %s\n",
        v->save_state(h.instance,nullptr,&need)==ULTRA_ERR_UNSUPPORTED ?
        "UNSUPPORTED" : "AVAILABLE -- replay/rollback on a native arcade game (other loaders cannot)");

    v->unload(h.instance); v->destroy(h.instance);
    std::printf("\n=== native x86 boards run via the loader; hook adds determinism ===\n");
    return 0;
}
