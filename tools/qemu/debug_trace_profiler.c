#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

typedef struct {
    /* Instrument the half-open guest PC range [start, end).
     * For example, if:
     *   nm -S --defined-only build/image.elf
     * prints:
     *   80004a54 000000f0 T debug_trace_event
     * then:
     *   start = 0x80004a54
     *   size  = 0x000000f0
     *   end   = start + size = 0x80004b44
     */
    uint64_t start;
    uint64_t end;
    uint64_t insn_count;
    uint64_t store_count;
} profiler_state_t;

static profiler_state_t g_state;

static bool parse_u64_arg(const char *value, uint64_t *out)
{
    char *end = NULL;
    size_t len;

    if (!value || value[0] == '\0' || value[0] == '+' || value[0] == '-') {
        return false;
    }

    len = strlen(value);
    if (isspace((unsigned char) value[0]) ||
        isspace((unsigned char) value[len - 1])) {
        return false;
    }

    errno = 0;
    uint64_t parsed = strtoull(value, &end, 0);

    if (errno == ERANGE || !end || end == value || *end != '\0') {
        return false;
    }

    *out = parsed;
    return true;
}

static void insn_exec_cb(unsigned int vcpu_index, void *userdata)
{
    (void) vcpu_index;
    (void) userdata;
    __atomic_fetch_add(&g_state.insn_count, 1, __ATOMIC_RELAXED);
}

static void mem_exec_cb(unsigned int vcpu_index,
                        qemu_plugin_meminfo_t info,
                        uint64_t vaddr,
                        void *userdata)
{
    (void) vcpu_index;
    (void) vaddr;
    (void) userdata;

    if (qemu_plugin_mem_is_store(info)) {
        __atomic_fetch_add(&g_state.store_count, 1, __ATOMIC_RELAXED);
    }
}

static void plugin_exit_cb(qemu_plugin_id_t id, void *userdata)
{
    (void) id;
    (void) userdata;

    fprintf(stderr,
            "debug_trace_profiler: start=0x%" PRIx64 " end=0x%" PRIx64
            " insn_count=%" PRIu64 " store_count=%" PRIu64 "\n",
            g_state.start, g_state.end,
            __atomic_load_n(&g_state.insn_count, __ATOMIC_RELAXED),
            __atomic_load_n(&g_state.store_count, __ATOMIC_RELAXED));
}

static void tb_trans_cb(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    size_t n_insns = qemu_plugin_tb_n_insns(tb);

    for (size_t i = 0; i < n_insns; ++i) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        uint64_t insn_pc = qemu_plugin_insn_vaddr(insn);

        if (insn_pc < g_state.start || insn_pc >= g_state.end) {
            continue;
        }

        qemu_plugin_register_vcpu_insn_exec_cb(insn, insn_exec_cb,
                                               QEMU_PLUGIN_CB_NO_REGS, NULL);
        qemu_plugin_register_vcpu_mem_cb(
            insn, mem_exec_cb, QEMU_PLUGIN_CB_NO_REGS, QEMU_PLUGIN_MEM_W, NULL);
    }
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc,
                                           char **argv)
{
    bool have_start = false;
    bool have_end = false;

    (void) info;

    for (int i = 0; i < argc; ++i) {
        const char *arg = argv[i];
        const char *eq = arg ? strchr(arg, '=') : NULL;
        size_t name_len;

        if (!eq) {
            continue;
        }

        name_len = (size_t) (eq - arg);
        if (name_len == 5 && strncmp(arg, "start", name_len) == 0) {
            have_start = parse_u64_arg(eq + 1, &g_state.start);
        } else if (name_len == 3 && strncmp(arg, "end", name_len) == 0) {
            have_end = parse_u64_arg(eq + 1, &g_state.end);
        }
    }

    if (!have_start || !have_end || g_state.start >= g_state.end) {
        fprintf(stderr,
                "debug_trace_profiler: expected start=0x... and end=0x...\n");
        return -1;
    }

    /* start/end are resolved outside the plugin from the target ELF symbol
     * table. end should be computed as start + size from `nm -S` output, so
     * the plugin can filter instructions with start <= PC < end.
     */

    qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans_cb);
    qemu_plugin_register_atexit_cb(id, plugin_exit_cb, NULL);
    return 0;
}
