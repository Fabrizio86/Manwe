//
// Created by Fabrizio Paino on 2022-01-19.
//

#ifndef YARN_WORKLOAD_H
#define YARN_WORKLOAD_H

namespace YarnBall {

    enum Workload {
        Idle = 0,
        Busy = 35,
        Burdened = 70,
        Overburdened
    };

}

#endif //YARN_WORKLOAD_H
