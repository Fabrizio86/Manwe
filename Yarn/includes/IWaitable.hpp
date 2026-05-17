//
// Created by Fabrizio Paino on 2022-01-16.
//

#ifndef YARN_IWAITABLE_HPP
#define YARN_IWAITABLE_HPP

#include "ITask.hpp"
#include <string>

namespace YarnBall {

    /**
     * @class IWaitable
     * @brief A task whose completion can be observed by the submitter.
     *
     * The standard pattern is:
     *  @code
     *  auto wt = std::make_shared<MyWaitable>();
     *  YarnBall::post(wt);
     *  wt->wait();              // blocks until run() returns
     *  if (wt->hasFailed()) ... // inspect error state
     *  @endcode
     *
     * The shared_ptr is co-owned by the executor and the submitter; the
     * submitter may safely outlive the executor's reference.
     */
    class IWaitable : public ITask {
    public:
        virtual ~IWaitable() = default;

        /**
         * @brief Block until the task completes (successfully or with an exception).
         */
        virtual void wait() = 0;

        /**
         * @brief @c true if @c run terminated with an uncaught exception.
         */
        virtual bool hasFailed() = 0;

        /**
         * @brief Human-readable error message, populated when @c hasFailed returns true.
         */
        virtual std::string errorMessage() = 0;
    };

    /**
     * @brief Co-owning waitable handle.
     */
    using sIWaitable = std::shared_ptr<IWaitable>;

}

#endif //YARN_IWAITABLE_HPP
