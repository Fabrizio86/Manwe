//
// Created by Fabrizio Paino on 2022-01-16.
//

#include "Waitable.h"
#include "YarnBall.h"

namespace YarnBall {

    void Waitable::wait() {
        std::this_thread::yield();
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&done = this->done] { return done; });
    }

    bool Waitable::hasFailed() {
        return this->failed;
    }

    void Waitable::run() {
        this->operation();
        this->notifyDone();
    }

    void Waitable::notifyDone() {
        this->done = true;
        this->cv.notify_all();
    }

    void Waitable::exception(std::exception_ptr exception) {
        this->failed = true;

        try { std::rethrow_exception(exception); }
        catch (const std::exception &e) { this->error = e.what(); }
        catch (const std::string &e) { this->error = e; }
        catch (const char *e) { this->error = e; }
        catch (...) { this->error = "Unknown error"; }

        this->notifyDone();
    }

    std::string Waitable::errorMessage() {
        return this->error;
    }

    void Waitable::operation() {

    }
}