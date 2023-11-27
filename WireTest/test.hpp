//
// Created by Fabrizio Paino on 2023-03-13.
// Copyright (c) 2023 Aedifex Solutions Inc. All rights reserved.
//

#include "Epoc.hpp"

#include <iostream>

using namespace std;

namespace Telegraph {

    class EventObject : public iEventObject {
    public:
        ~EventObject() override = default;

        std::string name() override {
            return typeid(EventObject).name();
        }

        int i = 0;
    };

    class TestEvent : public iEvent {
    public:
        TestEvent() = default;

        ~TestEvent() override = default;

        std::string name() override {
            return typeid(TestEvent).name();
        }

        void trigger(iEventObject *data) override {
            static_assert(std::is_base_of<iEventObject, EventObject>::value, "data must inherit from iEventObject");
            std::cout << data->name() << std::endl;
            ((EventObject *) data)->i++;
        }

    };

    void test() {
        TestEvent test;
        EventObject object;

        Epoc::instance()->registerEvent(&test);

        cout << object.i << endl;

        Epoc::instance()->trigger<TestEvent>(&object);

        cout << object.i << endl;

        if (object.i != 1)
            throw "incorrect result";
    }

}