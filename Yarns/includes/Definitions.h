//
// Created by fabrizio on 2020-08-03.
//

#ifndef YARNS_DEFINITIONS_H
#define YARNS_DEFINITIONS_H

#include "iawaitable.hpp"
#include "itask.hpp"

#include <functional>
#include <memory>

namespace YarnBall {

    /// \brief Definition for a task
    using Task = std::function<void()>;

    ///\brief Shorthand helper shared pointers
    using sIWaitable = std::shared_ptr<IAwaitable>;

    ///\brief Shorthand helper shared pointers
    using sITask = std::shared_ptr<ITask>;

}

#endif //YARNS_DEFINITIONS_H
