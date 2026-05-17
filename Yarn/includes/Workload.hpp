//
// Created by Fabrizio Paino on 2022-01-19.
//

#ifndef YARN_WORKLOAD_HPP
#define YARN_WORKLOAD_HPP

namespace YarnBall {

    /**
     * @enum Workload
     * @brief Coarse-grained load buckets for a fiber, derived from its local
     *        deque occupancy. Numeric values double as the percentage upper
     *        bound for the band, so they can be compared with the computed
     *        percentage directly.
     *
     * Bands:
     *  - @c Idle         : 0% (deque is empty).
     *  - @c Busy         : 0% < occupancy <= 35%.
     *  - @c Burdened     : 35% < occupancy < 70%.
     *  - @c Overburdened : 70% <= occupancy <= 100%.
     */
    enum Workload {
        Idle = 0,
        Busy = 35,
        Burdened = 70,
        Overburdened = 100
    };

}

#endif //YARN_WORKLOAD_HPP
