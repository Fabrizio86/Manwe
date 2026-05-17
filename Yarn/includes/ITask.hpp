#ifndef ITASK_H
#define ITASK_H

#include "Definitions.hpp"

namespace YarnBall {

    /**
     * @class ITask
     * @brief Abstract unit of work executed by the pool.
     *
     * Lifetime contract:
     *  - When submitted via @c Yarn::run(uITask), ownership is transferred to
     *    the executor and the worker @c delete s the task after @c run.
     *  - When submitted via @c Yarn::run(sITask), the executor wraps it in an
     *    internal owning adapter; the caller's @c shared_ptr remains valid.
     *
     * Failure contract:
     *  - If @c run throws, the worker forwards the @c std::exception_ptr to
     *    @c exception. Secondary exceptions from @c exception are swallowed.
     */
    class ITask {
    public:
        /**
         * @brief Virtual destructor — required for owning-pointer deletion.
         */
        virtual ~ITask() = default;

        /**
         * @brief Body of the task. Invoked on a worker thread.
         */
        virtual void run() = 0;

        /**
         * @brief Called by the worker when @c run throws. Implementations may
         *        rethrow, log, or store the exception.
         */
        virtual void exception(std::exception_ptr exception) = 0;
    };

}

#endif
