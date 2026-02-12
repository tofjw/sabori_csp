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

bool Variable::remove_below(Domain::value_type threshold) {
    bool ok = domain_.remove_below(threshold);
    if (ok) sync_soa();
    return ok;
}

bool Variable::remove_above(Domain::value_type threshold) {
    bool ok = domain_.remove_above(threshold);
    if (ok) sync_soa();
    return ok;
}

void Variable::sync_soa() {
    if (!model_) return;
    auto& vd = model_->var_data(id_);
    vd.min = domain_.min().value_or(vd.min);
    vd.max = domain_.max().value_or(vd.max);
    vd.size = domain_.size();
}

} // namespace sabori_csp
