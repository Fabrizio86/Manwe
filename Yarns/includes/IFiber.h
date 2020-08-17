//
// Created by Fabrizio Paino on 2020-09-02.
//

#ifndef YARNS_IFIBER_H
#define YARNS_IFIBER_H

#include "Definitions.h"
#include "FiberState.h"

namespace YarnBall {

    class IFiber {
    public:
        ~IFiber() = default;

        virtual Workload addWork(sITask work) = 0;

        virtual size_t queueSize() = 0;
    };

}

#endif //YARNS_IFIBER_H
