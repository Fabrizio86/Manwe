//
// Created by Fabrizio Paino on 2020-08-16.
//

#ifndef YARNS_FIBER_H
#define YARNS_FIBER_H

#include "Definitions.h"
#include "BaseThread.h"
#include "IFiber.h"

namespace YarnBall {

    class Fiber final : public BaseThread, public IFiber {
    public:
        ///\brief prevent copy constructor
        Fiber(const Fiber &) = delete;

        /// \brief prevent move semantics
        Fiber(Fiber &&) = delete;

        explicit Fiber(uint upperLimit);

        ~Fiber();

        Workload addWork(sITask work) override;

        sITask stealWork();

    protected:
        void work() override;

        size_t queueSize() override;

        void clearQueue() override;

    private:

        std::deque<sITask> queue;
    };

}

#endif //YARNS_FIBER_H
