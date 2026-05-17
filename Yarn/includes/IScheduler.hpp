//
// Created by Fabrizio Paino on 2022-01-16.
//

#ifndef YARN_ISCHEDULER_HPP
#define YARN_ISCHEDULER_HPP

namespace YarnBall {

    /**
     * @class IScheduler
     * @brief Strategy for picking an index in some bounded range. Used by
     *        @c Yarn for victim selection in legacy paths and available for
     *        custom dispatch policies. The current Yarn primarily uses TLS
     *        dispatch and random-peer stealing, but a custom IScheduler can
     *        still be plugged in via @c Yarn::switchScheduler.
     */
    class IScheduler {
    public:
        virtual ~IScheduler() = default;

        /**
         * @brief Return an integer in @c [0, maxValue). Returning a negative
         *        value tells the caller to skip selection (used to signal
         *        "no valid choice" when @p maxValue is zero).
         */
        virtual int ThreadIndex(int maxValue) = 0;
    };

}

#endif //YARN_ISCHEDULER_HPP
