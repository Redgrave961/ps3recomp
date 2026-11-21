#ifndef PS3EMU_PS3_ENV_ONCE_H
#define PS3EMU_PS3_ENV_ONCE_H

#include <stdlib.h>
#include <string.h>

#define PS3_ENV_ONCE_INLINE static inline

#define PS3_ENV_ONCE_SLOTS    64
#define PS3_ENV_ONCE_NAME_MAX 48

typedef struct {
    volatile long state;
    unsigned hash;
    char name[PS3_ENV_ONCE_NAME_MAX];
    const char* value;
} ps3_env_once_slot;

PS3_ENV_ONCE_INLINE long ps3_env_once_load(volatile long* p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
PS3_ENV_ONCE_INLINE void ps3_env_once_publish(volatile long* p, long v)
{
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}
PS3_ENV_ONCE_INLINE int ps3_env_once_claim(volatile long* p)
{
    long expected = 0;
    return __atomic_compare_exchange_n(p, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

PS3_ENV_ONCE_INLINE const char* ps3_env_once(const char* name)
{
    static ps3_env_once_slot slots[PS3_ENV_ONCE_SLOTS];
    if (!name) return NULL;
    unsigned hash = 2166136261u;
    size_t len = 0;
    for (const char* c = name; *c; c++, len++) hash = (hash ^ (unsigned char)*c) * 16777619u;
    if (len >= PS3_ENV_ONCE_NAME_MAX) return getenv(name);
    for (unsigned i = 0; i < PS3_ENV_ONCE_SLOTS; i++) {
        ps3_env_once_slot* s = &slots[(hash + i) & (PS3_ENV_ONCE_SLOTS - 1u)];
        long state = ps3_env_once_load(&s->state);
        if (state == 0) {
            if (ps3_env_once_claim(&s->state)) {
                s->hash = hash;
                memcpy(s->name, name, len + 1);
                s->value = getenv(name);
                ps3_env_once_publish(&s->state, 2);
                return s->value;
            }
            state = ps3_env_once_load(&s->state);
        }
        if (state != 2) return getenv(name);
        if (s->hash == hash && memcmp(s->name, name, len + 1) == 0) return s->value;
    }
    return getenv(name);
}

#endif
