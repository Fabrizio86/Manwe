#include <exception>
#include <iostream>
#include <sstream>
#include <utility>
#include "../Yarn/includes/ITask.hpp"
#include "../Yarn/includes/YarnBall.hpp"
#include "../Yarn/includes/Yarn.hpp"
#include "../Yarn/includes/Waitable.hpp"

using namespace std;

#define ONE_THOUSAND 1000
#define HUNDRED_THOUSANDS (ONE_THOUSAND * 100)
#define FIVE_HUNDRED_THOUSANDS (HUNDRED_THOUSANDS * 5)
#define ONE_MILLION (FIVE_HUNDRED_THOUSANDS * 2)

class Task : public YarnBall::ITask {
public:
    Task() {
        this->operation = [] {
            srand(time(NULL));
            int randCnt = rand() % 200 + 5;
            stringstream ss;

            for (int i = 0; i < randCnt; i++) {
                srand(time(NULL));
                int randDly = rand() % 10 + 3;
                this_thread::sleep_for(chrono::milliseconds(randDly));

                ss << i << ", ";
            }

            string final = ss.str();
            cout << "Task completed string gen" << endl;
        };
    }

    explicit Task(YarnBall::Operation operation) : operation(std::move(operation)) {
    }

    ~Task() override = default;

    ///\brief The method called inside the thread
    void run() override {
        this->operation();
    }

    void exception(std::exception_ptr exception) override {
        cout << "exception triggered" << endl;
    }

private:
    YarnBall::Operation operation;
};

class WaitableTask : public YarnBall::Waitable {
public:
    void operation() override {
        srand(time(NULL));
        int randDly = rand() % 500 + 1;
        this_thread::sleep_for(chrono::milliseconds(randDly));

        cout << "before editing: " << txt << endl;
        txt += " edited in thread";
    }

    std::string txt;
};

int main() {
    YarnBall::Run(std::make_shared<Task>());
    YarnBall::Run(std::make_shared<Task>([] { cout << "Hi there" << endl; }));

    this_thread::sleep_for(chrono::seconds(3));

    cout << "starting the waitable" << endl;

    auto wt = std::make_shared<WaitableTask>();
    wt->txt = "hi there some random text";
    YarnBall::Post(wt);

    wt->wait();

    cout << "after editing: " << wt->txt << endl;

    cout << "done waiting, press any key to continue: ";

    char c;
    cin >> c;

    for (int i = 0; i < HUNDRED_THOUSANDS; ++i) {
        YarnBall::Run(std::make_shared<Task>());

        if (i == (ONE_THOUSAND * 3) || i == (ONE_THOUSAND * 6) || i == (ONE_THOUSAND * 9) || i == (ONE_THOUSAND * 19)) {
            cout << "pause point, press to continue: ";
            cin >> c;
        }
    }

    int i;

    cout << "Done waiting, press a key to exit: ";
    cin >> i;

    return 0;
}
