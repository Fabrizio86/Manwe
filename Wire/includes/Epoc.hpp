//
// Created by Fabrizio Paino on 2023-03-11.
// Copyright (c) 2023 Aedifex Solutions Inc. All rights reserved.
//

#ifndef MANWE_EPOC_HPP
#define MANWE_EPOC_HPP

#include "iEvent.hpp"

#include <string>
#include <typeinfo>
#include <unordered_map>

namespace Telegraph {

    class Epoc final {
    public:
        static Epoc *instance() {
            static Epoc instance;
            return &instance;
        }

        void registerEvent(iEvent* event) {
            if(event == nullptr) return;

            this->events[event->name()] = event;
        }

        template<class Event>
        void trigger(iEventObject* data) {
            static_assert(std::is_base_of<iEvent, Event>::value, "Event must inherit from iEvent");
            this->events[typeid(Event).name()]->trigger(data);
        }

    private:
        Epoc() = default;

        std::unordered_map<std::string, iEvent*> events;
    };

}

#endif //MANWE_EPOC_HPP
