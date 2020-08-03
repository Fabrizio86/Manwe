#include "waitable.hpp"
#include "yarns.hpp"

namespace YarnBall {

    // Class representing the awaitable object.
    // If you need to wait for an operation to complete,
    // this will implement the wait pattern for the thread pool execution.
    class Waitable final : public IWaitable {
    public:
        /// \brief Constructor accepting instance of Task
        /// \param task
        explicit Waitable(Task task) : done{false}, task{std::move(task)} {}

        /// \brief Default destructor
        ~Waitable() override = default;

        /// \brief tells the caller to wait for the task to execute
        void wait() override {
            std::this_thread::yield();
            std::unique_lock<std::mutex> lk(mu);
            cv.wait(lk, [this] { return this->done; });
        }

        /// \brief executes the task
        void run() override {
            this->task();
            this->Done();
        }

        /// \brief tells if the task has completed
        void Done() {
            if (!this->done) {
                this->done = true;
                this->cv.notify_all();
            }
        }

        /// \brief captures the exception
        void exception(std::exception_ptr e) override {
            this->ex = e;
            this->Done();
        }

        /// \brief returns the exception thrown, if any or nullptr
        std::exception_ptr getException() override {
            return this->ex;
        }

    private:
        bool done;
        Task task;
        std::mutex mu;
        std::condition_variable cv;
        std::exception_ptr ex;
    };

    sIWaitable Promise(Task task) {
        sIWaitable wt = std::make_shared<Waitable>(task);
        Yarns::instance()->addTask(wt);
        return wt;
    }
}
