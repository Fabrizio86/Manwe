//
// Created by Fabrizio Paino on 2022-01-16.
//

#ifndef YARN_WAITABLE_HPP
#define YARN_WAITABLE_HPP

#include "Yarn.hpp"
#include "ITask.hpp"
#include "IWaitable.hpp"

namespace YarnBall {

    class Waitable : public IWaitable {
    public:
        Waitable() = default;

        ~Waitable() = default;

        void wait() override;

        bool hasFailed() override;

        std::string errorMessage() override;

        virtual void operation();

    private:
        void run() override;

        void notifyDone();

        void exception(std::exception_ptr exception) override;

        void interrupted();

        bool done = false;
        bool failed = false;
        std::mutex mu;
        std::condition_variable cv;
        std::string error;
    };
}

#endif //YARN_WAITABLE_HPP
