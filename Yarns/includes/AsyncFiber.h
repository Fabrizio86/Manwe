//
// Created by Fabrizio Paino on 2020-08-25.
//

#ifndef YARNS_ASYNCFIBER_H
#define YARNS_ASYNCFIBER_H

#include "Definitions.h"
#include "BaseThread.h"

namespace YarnBall {

    class AsyncFiber final : public BaseThread {
    public:
        ///\brief prevent copy constructor
        AsyncFiber(const AsyncFiber &) = delete;

        /// \brief prevent move semantics
        AsyncFiber(AsyncFiber &&) = delete;

        explicit AsyncFiber(uint upperLimit);

        Workload addWork(Task work);

        Task stealWork();

    protected:
        void work() override;

        size_t queueSize() override;

        void clearQueue() override;

    private:
        std::deque<Task> queue;
    };

}

#endif //YARNS_ASYNCFIBER_H
