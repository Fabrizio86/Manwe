//
// Created by Fabrizio Paino on 2022-01-16.
//

#ifndef YARN_ISCHEDULER_H
#define YARN_ISCHEDULER_H

namespace YarnBall {

    class IScheduler {
    public:
        virtual ~IScheduler() = default;

        virtual int ThreadIndex(int maxValue) = 0;
    };

}

#endif //YARN_ISCHEDULER_H
