//
// Created by Fabrizio Paino on 2022-01-16.
//

#ifndef YARN_WAITABLE_H
#define YARN_WAITABLE_H

#include "Yarn.h"
#include "IWaitable.h"

namespace YarnBall {

    class Waitable final : public IWaitable, public ITask {
    public:
        Waitable(YarnBall::Operation operation);

        ~Waitable() = default;

        void wait() override;

        bool hasFailed() override;

        std::string errorMessage() override;

    private:
        void run() override;

        void notifyComplition();

        void exception(std::exception_ptr exception) override;

        void interrupted();

        bool done = false;
        bool failed = false;
        YarnBall::Operation operation;
        std::mutex mu;
        std::condition_variable cv;
        std::string error;
    };
}

#endif //YARN_WAITABLE_H
