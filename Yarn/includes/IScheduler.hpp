//
// Created by Fabrizio Paino on 2022-01-16.
//

#ifndef YARN_ISCHEDULER_HPP
#define YARN_ISCHEDULER_HPP

namespace YarnBall {

    class IScheduler {
    public:
        virtual ~IScheduler() = default;

        virtual int ThreadIndex(int maxValue) = 0;
    };

}

#endif //YARN_ISCHEDULER_HPP
