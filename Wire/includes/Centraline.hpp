//
// Created by Fabrizio Paino on 2023-02-18.
// Copyright (c) 2023 Aedifex Solutions Inc. All rights reserved.
//

#ifndef MANWE_CENTRALINE_H
#define MANWE_CENTRALINE_H

#include <string>
#include <unordered_map>
#include "YarnBall.hpp"

namespace Telegraph {

    template<typename... Args>
    class Centraline final {
    public:
        static Centraline &instance() {
            static Centraline instance;
            return instance;
        }

        void connect(std::function<void(Args...)> slot, bool directCall = true) {
            store.push_back(slot, true);
        }

        void disconnect(void (*slot)(Args...)) {
            for (auto it = store.begin(); it != store.end(); ++it) {
                if (*it == slot) {
                    store.erase(it);
                    break;
                }
            }
        }

        void emit(Args... args) {
            for (const auto &slot: this->store) {
                if (std::get<1>(slot))
                    std::get<0>(slot)(args...);
                else
                    YarnBall::Run(std::make_shared<Task>([&] { std::get<0>(slot)(args...); }));
            }
        }

    private:
        Centraline() = default;

        using WireInfo = std::tuple<std::function<void(Args...)>, bool>;

        std::vector<WireInfo> store;

        // internal class to post jobs on the Yarn pool
        class Task : public YarnBall::ITask {
        public:
            explicit Task(YarnBall::Operation operation) : operation(std::move(operation)) {}

            ~Task() override = default;

            ///\brief The method called inside the thread
            void run() override {
                this->operation();
            }

            void exception(std::exception_ptr exception) override {}

        private:
            YarnBall::Operation operation;

        };
    };
}

#endif //MANWE_CENTRALINE_H
