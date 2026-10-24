#ifndef CONTACT_FRESHNESS_H
#define CONTACT_FRESHNESS_H

#include <stdint.h>

template <class Contact>
void expire_contacts(Contact *tracked, int &count, uint32_t now, uint32_t age_ms) {
    int write = 0;
    for (int i = 0; i < count; i++) {
        if ((uint32_t)(now - tracked[i].fix_ms) >= age_ms) continue;
        if (write != i) tracked[write] = tracked[i];
        write++;
    }
    count = write;
}

#endif // CONTACT_FRESHNESS_H
