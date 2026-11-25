// The registry of command buffers (cmdbuf.c g_command_buffers): a doubly linked list, newest first, through
// NvnCmdBuf.registry_next / registry_prev. The caller holds g_shadow_lock. Needs NvnCmdBuf (nvn.h) and <stdbool.h>.
// Kept in a header of its own so that tools/test_cb_registry.py can compile exactly this code.
#pragma once

static inline void cb_registry_add(NvnCmdBuf **head, NvnCmdBuf *cb) {
    cb->registry_prev = NULL;
    cb->registry_next = *head;
    if (*head) (*head)->registry_prev = cb;
    *head = cb;
}

// Takes cb out of the list: a buffer the game finalizes after it was already taken out, or one that was never
// registered (a NULL host pointer in its guest object), changes nothing. unlink_o1 (cpuopts bit 2048): by its own two
// links, checked first (a buffer that was never registered, or was taken out already, takes the walk, which finds
// nothing: its links are NULL, and a NULL previous link only counts for the head); otherwise by a walk
// from the head, which in a registry of every list the game keeps costs the more the older the buffer is.
// The links are kept either way, so the two ways can be switched between at any time.
static inline void cb_registry_remove(NvnCmdBuf **head, NvnCmdBuf *cb, bool unlink_o1) {
    if (!cb) return;
    if (unlink_o1 && (cb->registry_prev ? cb->registry_prev->registry_next == cb : *head == cb)) {
        if (cb->registry_prev) cb->registry_prev->registry_next = cb->registry_next;
        else *head = cb->registry_next;
        if (cb->registry_next) cb->registry_next->registry_prev = cb->registry_prev;
        cb->registry_next = cb->registry_prev = NULL;
        return;
    }
    NvnCmdBuf *prev = NULL;
    for (NvnCmdBuf *it = *head; it; prev = it, it = it->registry_next) {
        if (it != cb) continue;
        if (prev) prev->registry_next = it->registry_next;
        else *head = it->registry_next;
        if (it->registry_next) it->registry_next->registry_prev = prev;
        it->registry_next = it->registry_prev = NULL;
        return;
    }
}
