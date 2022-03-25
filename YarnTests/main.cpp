#include <iostream>
#include <utility>
#include "../Yarn/includes/ITask.h"
#include "../Yarn/includes/YarnBall.h"
#include "../Yarn/includes/Yarn.h"

using namespace std;

class Task : public YarnBall::ITask {
public:
    Task() {
        this->operation = [] {
            this_thread::sleep_for(chrono::milliseconds (300));
        };
    }

    explicit Task(YarnBall::Operation operation) : operation(std::move(operation)) {}

    ~Task() override = default;

    ///\brief The method called inside the thread
    void run() override {
        this->operation();
    }

    void exception(std::exception_ptr exception) override {

    }

private:
    YarnBall::Operation operation;

};

int main() {
    YarnBall::Run(new Task());
    YarnBall::Run(new Task([] { cout << "Hi there" << endl; }));

    auto wt = YarnBall::Post([] {
        for (int i = 0; i < 100000; i++) {
            cout << i << endl;
        }
    });

    string txt = "hi there some random text";
    auto wt2 = YarnBall::Post([&txt] {
        cout << txt << endl;
        txt += " edited in thread";
    });

    wt->wait();
    wt2->wait();

    for (int i = 0; i < 10000; ++i) {
        YarnBall::Run(new Task());
    }

    int i;

    cout << txt << endl;

    cout << "Done waiting" << endl;
    cin >> i;

    return 0;
}
