//
// Created by Fabrizio Paino on 2020-09-02.
//

#ifndef YARNS_IFIBER_H
#define YARNS_IFIBER_H

#include "FiberState.h"
#include "Definitions.h"

namespace YarnBall {

    class IFiber {
    public:
        ~IFiber() = default;

        virtual Workload addWork(ITask* work) = 0;

        virtual Workload addWork(sITask work) = 0;

        virtual void MarkAsync() = 0;

        virtual size_t queueSize() = 0;

        virtual ThreadId id() = 0;

        virtual void markAsTemp() = 0;

        virtual void detach() = 0;

        virtual void join() = 0;

        virtual Workload getWorkload() = 0;

        virtual sITask stealWork() = 0;
    };

}

#endif //YARNS_IFIBER_H
