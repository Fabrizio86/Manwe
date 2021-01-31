//
// Created by fabrizio on 2020-08-04.
//

#include "./Includes/catch.hpp"
#include "yarns.hpp"
#include "system.hpp"

#include <iostream>

using namespace std;

class TestTask : public YarnBall::ITask {
public:
    ~TestTask() override = default;

    void run() override {
        int j = 0;
        for (int i = 0; i < 1000000; ++i) {
            j += i;
        }
    }

    void exception(std::exception_ptr exception) override {
        cout << "Error" << endl;
    }
};

int main() {
    cout << YarnBall::Yarns::instance()->fiberSize() << endl
         << YarnBall::Yarns::instance()->aFiberSize() << endl;

    int ii = 0;
    cin >> ii;

    for (int i = 0; i < 100000; ++i) {
        if (i == 1000 || i == 10000 || i == 50000 || i == 500000) {
            cout << endl << YarnBall::Yarns::instance()->fiberSize() << endl
                 << YarnBall::Yarns::instance()->aFiberSize() << endl;

            cout << "...resuming?" << endl;

            cin >> ii;

            cout << endl << YarnBall::Yarns::instance()->fiberSize() << endl
                 << YarnBall::Yarns::instance()->aFiberSize() << endl;
        }

        YarnBall::Submit<TestTask>();
    }

    cout << "Ready to continue? ";
    cin >> ii;

    cout << YarnBall::Yarns::instance()->fiberSize() << endl
         << YarnBall::Yarns::instance()->aFiberSize() << endl;

    return 0;
}