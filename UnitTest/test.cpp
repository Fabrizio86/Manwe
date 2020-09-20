//
// Created by fabrizio on 2020-08-04.
//

#include "./Includes/catch.hpp"
#include "yarns.hpp"
#include "system.hpp"

#include <chrono>
#include <iostream>
#include <thread>

using namespace std;

class TestTask : public YarnBall::ITask {
public:
    ~TestTask() override = default;

    void run() override {
        int j = 0;
        for(int i = 0; i < 100000; ++i) {
            j += i;
        }

        cout << "done" << endl;
    }

    void exception(std::exception_ptr exception) override {
        cout << "Error" << endl;
    }
};

int main() {
    auto started = std::chrono::high_resolution_clock::now();

    for(int i = 0; i < 500000; ++i) {
        if(i == 1000 || i == 5000 || i == 10000 || i == 50000)
         this_thread::sleep_for(chrono::seconds (5));
        YarnBall::Submit<TestTask>();
    }

    auto done = std::chrono::high_resolution_clock::now();
    auto time = std::chrono::duration_cast<std::chrono::milliseconds>(done-started).count();

    cout << "------> Done in: " << time << "ms" << endl;

    int i = 0;
    cin >> i;

    ///YarnBall::Yarns::instance()->stop();
    return 0;
}