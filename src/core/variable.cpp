#include "sabori_csp/variable.hpp"
#include "sabori_csp/model.hpp"

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

bool Variable::assign(Domain::value_type value) {
    bool ok = domain_.assign(value);
    if (ok) sync_soa();
    return ok;
}

bool Variable::remove(Domain::value_type value) {
    bool ok = domain_.remove(value);
    if (ok) sync_soa();
    return ok;
}

void Variable::sync_soa() {
    if (!model_) return;
    auto& mins = model_->mins();
    auto& maxs = model_->maxs();
    auto& sizes = model_->sizes();
    mins[id_] = domain_.min().value_or(mins[id_]);
    maxs[id_] = domain_.max().value_or(maxs[id_]);
    sizes[id_] = domain_.size();
}

bool Variable::is_assigned() const {
    return domain_.is_singleton();
}

std::optional<Domain::value_type> Variable::assigned_value() const {
    if (is_assigned()) {
        return min();
    }
    return std::nullopt;
}

} // namespace sabori_csp
