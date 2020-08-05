#ifndef WAITABLE_H
#define WAITABLE_H

#include "Definitions.h"
#include "iwaitable.hpp"
#include <mutex>

namespace YarnBall {

    sIWaitable Promise(const Task& task);

}

#endif // WAITABLE_H
