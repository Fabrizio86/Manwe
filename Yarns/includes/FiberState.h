//
// Created by Fabrizio Paino on 2020-08-17.
//

#ifndef YARNS_FIBERSTATE_H
#define YARNS_FIBERSTATE_H

namespace YarnBall {

    /// \brief The thread state
    enum State {
        Idle,
        Running,
        Waiting,
        Error,
        Aborting
    };

    enum Workload {
        Normal = 0,
        Taxed = 5,
        Exhausted = 11,
    };

}

#endif //YARNS_FIBERSTATE_H
