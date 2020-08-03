#ifndef WAITABLE_H
#define WAITABLE_H

#include "iwaitable.hpp"
#include <mutex>

namespace YarnBall {

    sIWaitable Promise(Task task);

}

#endif // WAITABLE_H
