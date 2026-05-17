//
// Created by Fabrizio Paino on 2022-01-16.
//

#ifndef YARN_YARNBALL_HPP
#define YARN_YARNBALL_HPP

#include <memory>

#include <utility>

#include "ITask.hpp"
#include "IWaitable.hpp"
#include "Yarn.hpp"

namespace YarnBall {

    /**
     * @brief Submit an owned task to the pool. Preferred entry point — zero
     *        reference-count traffic, ownership transferred to the executor.
     * @param task Task to run. @c nullptr is ignored.
     */
    void run(uITask task);

    /**
     * @brief Submit a co-owned task to the pool. The caller may retain their
     *        @c sITask reference (useful for waitables / status observation).
     */
    void run(sITask task);

    /**
     * @brief Submit an arbitrary invocable. Equivalent to constructing a
     *        small pooled @c ITask wrapper around @p fn and routing it
     *        through @c run(uITask). Cheaper than building your own
     *        @c ITask subclass; allocates from the small-object pool so
     *        the steady state never goes through @c malloc.
     */
    template<typename F,
             typename = std::enable_if_t<!std::is_convertible_v<F &&, uITask> &&
                                          !std::is_convertible_v<F &&, sITask>>>
    inline void run(F &&fn) {
        Yarn::instance()->run(std::forward<F>(fn));
    }

    /**
     * @brief Submit a waitable. Functionally identical to @c Run(sITask),
     *        kept as a named entry for caller intent.
     */
    void post(sIWaitable operation);

}

#endif //YARN_YARNBALL_HPP
