#include "sabori_csp/variable.hpp"

namespace sabori_csp {

Variable::Variable(std::string name, Domain domain)
    : name_(std::move(name)), domain_(std::move(domain)) {}

const std::string& Variable::name() const {
    return name_;
}

Domain& Variable::domain() {
    return domain_;
}

const Domain& Variable::domain() const {
    return domain_;
}

bool Variable::is_assigned() const {
    return domain_.is_singleton();
}

std::optional<Domain::value_type> Variable::assigned_value() const {
    if (is_assigned()) {
        return domain_.min();
    }
    return std::nullopt;
}

} // namespace sabori_csp
