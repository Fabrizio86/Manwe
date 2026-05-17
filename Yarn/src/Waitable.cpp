//
// Created by Fabrizio Paino on 2022-01-16.
//

#include "Waitable.hpp"

#include <exception>

#include "YarnBall.hpp"

namespace YarnBall {

    void Waitable::wait() {
        std::unique_lock<std::mutex> lk(this->mu);
        this->cv.wait(lk, [this] { return this->done; });
    }

    bool Waitable::hasFailed() {
        std::lock_guard<std::mutex> lk(this->mu);
        return this->failed;
    }

    void Waitable::run() {
        this->operation();
        this->notifyDone();
    }

    void Waitable::notifyDone() {
        {
            std::lock_guard<std::mutex> lk(this->mu);
            this->done = true;
        }
        this->cv.notify_all();
    }

    void Waitable::exception(std::exception_ptr exception) {
        {
            std::lock_guard<std::mutex> lk(this->mu);
            this->failed = true;
            try { std::rethrow_exception(exception); }
            catch (const std::exception &e) { this->error = e.what(); }
            catch (const std::string &e) { this->error = e; }
            catch (const char *e) { this->error = e; }
            catch (...) { this->error = "Unknown error"; }
            this->done = true;
        }
        this->cv.notify_all();
    }

    std::string Waitable::errorMessage() {
        std::lock_guard<std::mutex> lk(this->mu);
        return this->error;
    }

    void Waitable::operation() {
    }
}
