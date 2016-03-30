#ifndef __SPIN_H
#define __SPIN_H

typedef volatile unsigned int spin_t;

void spin_init();
void spin_lock(spin_t *lock);
void spin_unlock(spin_t *lock);

#endif
