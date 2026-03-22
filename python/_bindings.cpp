#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>

#include "sabori_csp/domain.hpp"
#include "sabori_csp/variable.hpp"
#include "sabori_csp/model.hpp"
#include "sabori_csp/solver.hpp"
#include "sabori_csp/constraint.hpp"
#include "sabori_csp/constraints/arithmetic.hpp"
#include "sabori_csp/constraints/comparison.hpp"
#include "sabori_csp/constraints/logical.hpp"
#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/constraints/all_different_gac.hpp"

namespace py = pybind11;
using namespace sabori_csp;

PYBIND11_MODULE(_sabori_csp, m) {
    m.doc() = "Low-level Python bindings for sabori_csp constraint solver";

    // ---- Domain ----
    py::class_<Domain>(m, "Domain")
        .def(py::init<>())
        .def(py::init<Domain::value_type, Domain::value_type>(),
             py::arg("min"), py::arg("max"))
        .def(py::init<std::vector<Domain::value_type>>(),
             py::arg("values"))
        .def("empty", &Domain::empty)
        .def("size", &Domain::size)
        .def("min", &Domain::min)
        .def("max", &Domain::max)
        .def("contains", &Domain::contains, py::arg("value"))
        .def("values", &Domain::values)
        .def("is_singleton", &Domain::is_singleton)
        .def("__repr__", [](const Domain& d) {
            if (d.empty()) return std::string("Domain({})");
            auto vals = d.values();
            if (vals.size() <= 10) {
                std::string s = "Domain({";
                for (size_t i = 0; i < vals.size(); ++i) {
                    if (i > 0) s += ", ";
                    s += std::to_string(vals[i]);
                }
                s += "})";
                return s;
            }
            return std::string("Domain(") + std::to_string(*d.min()) +
                   ".." + std::to_string(*d.max()) +
                   ", size=" + std::to_string(d.size()) + ")";
        });

    // ---- Variable ----
    py::class_<Variable>(m, "Variable")
        .def("id", &Variable::id)
        .def("name", &Variable::name)
        .def("min", &Variable::min)
        .def("max", &Variable::max)
        .def("is_assigned", &Variable::is_assigned)
        .def("assigned_value", &Variable::assigned_value)
        .def("domain", py::overload_cast<>(&Variable::domain),
             py::return_value_policy::reference_internal)
        .def("__repr__", [](const Variable& v) {
            std::string s = "Variable('" + v.name() + "', ";
            if (v.is_assigned()) {
                s += "=" + std::to_string(*v.assigned_value());
            } else {
                s += std::to_string(v.min()) + ".." + std::to_string(v.max());
            }
            s += ")";
            return s;
        });

    // ---- Model ----
    py::class_<Model>(m, "Model")
        .def(py::init<>())
        .def("create_variable",
             py::overload_cast<std::string, Domain::value_type, Domain::value_type>(
                 &Model::create_variable),
             py::arg("name"), py::arg("min"), py::arg("max"),
             py::return_value_policy::reference_internal)
        .def("create_variable",
             py::overload_cast<std::string, Domain>(&Model::create_variable),
             py::arg("name"), py::arg("domain"),
             py::return_value_policy::reference_internal)
        .def("create_variable",
             py::overload_cast<std::string, Domain::value_type>(&Model::create_variable),
             py::arg("name"), py::arg("value"),
             py::return_value_policy::reference_internal)
        .def("create_variable",
             py::overload_cast<std::string, std::vector<Domain::value_type>>(
                 &Model::create_variable),
             py::arg("name"), py::arg("values"),
             py::return_value_policy::reference_internal)
        .def("add_constraint", &Model::add_constraint, py::arg("constraint"))
        .def("variables", [](const Model& model) {
            std::vector<Variable*> result;
            const auto& vars = model.variables();
            result.reserve(vars.size());
            for (const auto& v : vars) {
                result.push_back(v.get());
            }
            return result;
        }, py::return_value_policy::reference, py::keep_alive<0, 1>())
        .def("variable",
             py::overload_cast<size_t>(&Model::variable, py::const_),
             py::arg("id"),
             py::return_value_policy::reference_internal)
        .def("variable",
             py::overload_cast<const std::string&>(&Model::variable, py::const_),
             py::arg("name"),
             py::return_value_policy::reference_internal)
        .def("set_defined_var", &Model::set_defined_var, py::arg("var_idx"))
        .def("set_no_bisect", &Model::set_no_bisect, py::arg("var_idx"))
        .def("__repr__", [](const Model& m) {
            return "Model(vars=" + std::to_string(m.variables().size()) +
                   ", constraints=" + std::to_string(m.constraints().size()) + ")";
        });

    // ---- SolverStats ----
    py::class_<SolverStats>(m, "SolverStats")
        .def_readonly("max_depth", &SolverStats::max_depth)
        .def_readonly("depth_sum", &SolverStats::depth_sum)
        .def_readonly("depth_count", &SolverStats::depth_count)
        .def_readonly("restart_count", &SolverStats::restart_count)
        .def_readonly("fail_count", &SolverStats::fail_count)
        .def_readonly("nogood_count", &SolverStats::nogood_count)
        .def_readonly("nogood_check_count", &SolverStats::nogood_check_count)
        .def_readonly("nogood_prune_count", &SolverStats::nogood_prune_count)
        .def_readonly("nogood_domain_count", &SolverStats::nogood_domain_count)
        .def_readonly("nogood_instantiate_count", &SolverStats::nogood_instantiate_count)
        .def_readonly("nogoods_size", &SolverStats::nogoods_size)
        .def_readonly("unit_nogoods_size", &SolverStats::unit_nogoods_size)
        .def_readonly("bisect_count", &SolverStats::bisect_count)
        .def_readonly("enumerate_count", &SolverStats::enumerate_count)
        .def("__repr__", [](const SolverStats& s) {
            return "SolverStats(fails=" + std::to_string(s.fail_count) +
                   ", restarts=" + std::to_string(s.restart_count) +
                   ", nogoods=" + std::to_string(s.nogood_count) + ")";
        });

    // ---- Solver ----
    py::class_<Solver>(m, "Solver")
        .def(py::init<>())
        .def("solve", &Solver::solve, py::arg("model"),
             py::call_guard<py::gil_scoped_release>())
        .def("solve_all", &Solver::solve_all,
             py::arg("model"), py::arg("callback"),
             py::call_guard<py::gil_scoped_release>())
        .def("solve_optimize", &Solver::solve_optimize,
             py::arg("model"), py::arg("obj_var_idx"), py::arg("minimize"),
             py::arg("on_improve") = nullptr,
             py::call_guard<py::gil_scoped_release>())
        .def("stats", &Solver::stats, py::return_value_policy::reference_internal)
        .def("set_nogood_learning", &Solver::set_nogood_learning, py::arg("enabled"))
        .def("set_restart_enabled", &Solver::set_restart_enabled, py::arg("enabled"))
        .def("set_activity_selection", &Solver::set_activity_selection, py::arg("enabled"))
        .def("set_activity_first", &Solver::set_activity_first, py::arg("enabled"))
        .def("set_bisection_threshold", &Solver::set_bisection_threshold, py::arg("threshold"))
        .def("set_verbose", &Solver::set_verbose, py::arg("enabled"))
        .def("set_community_analysis", &Solver::set_community_analysis, py::arg("enabled"))
        .def("stop", &Solver::stop)
        .def("reset_stop", &Solver::reset_stop)
        .def("is_stopped", &Solver::is_stopped);

    // ---- Constraint base ----
    py::class_<Constraint, std::shared_ptr<Constraint>>(m, "Constraint")
        .def("name", &Constraint::name)
        .def("id", &Constraint::id)
        .def("var_ids", &Constraint::var_ids_ref);

    // ---- Comparison constraints ----
    py::class_<IntEqConstraint, Constraint, std::shared_ptr<IntEqConstraint>>(
            m, "IntEqConstraint")
        .def(py::init<VariablePtr, VariablePtr>(), py::arg("x"), py::arg("y"));

    py::class_<IntNeConstraint, Constraint, std::shared_ptr<IntNeConstraint>>(
            m, "IntNeConstraint")
        .def(py::init<VariablePtr, VariablePtr>(), py::arg("x"), py::arg("y"));

    py::class_<IntLtConstraint, Constraint, std::shared_ptr<IntLtConstraint>>(
            m, "IntLtConstraint")
        .def(py::init<VariablePtr, VariablePtr>(), py::arg("x"), py::arg("y"));

    py::class_<IntLeConstraint, Constraint, std::shared_ptr<IntLeConstraint>>(
            m, "IntLeConstraint")
        .def(py::init<VariablePtr, VariablePtr>(), py::arg("x"), py::arg("y"));

    py::class_<IntMaxConstraint, Constraint, std::shared_ptr<IntMaxConstraint>>(
            m, "IntMaxConstraint")
        .def(py::init<VariablePtr, VariablePtr, VariablePtr>(),
             py::arg("x"), py::arg("y"), py::arg("m"));

    py::class_<IntMinConstraint, Constraint, std::shared_ptr<IntMinConstraint>>(
            m, "IntMinConstraint")
        .def(py::init<VariablePtr, VariablePtr, VariablePtr>(),
             py::arg("x"), py::arg("y"), py::arg("m"));

    py::class_<IntEqReifConstraint, Constraint, std::shared_ptr<IntEqReifConstraint>>(
            m, "IntEqReifConstraint")
        .def(py::init<VariablePtr, VariablePtr, VariablePtr>(),
             py::arg("x"), py::arg("y"), py::arg("b"));

    py::class_<IntNeReifConstraint, Constraint, std::shared_ptr<IntNeReifConstraint>>(
            m, "IntNeReifConstraint")
        .def(py::init<VariablePtr, VariablePtr, VariablePtr>(),
             py::arg("x"), py::arg("y"), py::arg("b"));

    py::class_<IntLeReifConstraint, Constraint, std::shared_ptr<IntLeReifConstraint>>(
            m, "IntLeReifConstraint")
        .def(py::init<VariablePtr, VariablePtr, VariablePtr>(),
             py::arg("x"), py::arg("y"), py::arg("b"));

    py::class_<IntEqImpConstraint, Constraint, std::shared_ptr<IntEqImpConstraint>>(
            m, "IntEqImpConstraint")
        .def(py::init<VariablePtr, VariablePtr, VariablePtr>(),
             py::arg("x"), py::arg("y"), py::arg("b"));

    // ---- Arithmetic constraints ----
    py::class_<IntTimesConstraint, Constraint, std::shared_ptr<IntTimesConstraint>>(
            m, "IntTimesConstraint")
        .def(py::init<VariablePtr, VariablePtr, VariablePtr>(),
             py::arg("x"), py::arg("y"), py::arg("z"));

    py::class_<IntAbsConstraint, Constraint, std::shared_ptr<IntAbsConstraint>>(
            m, "IntAbsConstraint")
        .def(py::init<VariablePtr, VariablePtr>(), py::arg("x"), py::arg("y"));

    py::class_<IntModConstraint, Constraint, std::shared_ptr<IntModConstraint>>(
            m, "IntModConstraint")
        .def(py::init<VariablePtr, VariablePtr, VariablePtr>(),
             py::arg("x"), py::arg("y"), py::arg("z"));

    py::class_<IntDivConstraint, Constraint, std::shared_ptr<IntDivConstraint>>(
            m, "IntDivConstraint")
        .def(py::init<VariablePtr, VariablePtr, VariablePtr>(),
             py::arg("x"), py::arg("y"), py::arg("z"));

    // ---- Linear constraints ----
    py::class_<IntLinEqConstraint, Constraint, std::shared_ptr<IntLinEqConstraint>>(
            m, "IntLinEqConstraint")
        .def(py::init<std::vector<int64_t>, std::vector<VariablePtr>, int64_t>(),
             py::arg("coeffs"), py::arg("vars"), py::arg("target"));

    py::class_<IntLinLeConstraint, Constraint, std::shared_ptr<IntLinLeConstraint>>(
            m, "IntLinLeConstraint")
        .def(py::init<std::vector<int64_t>, std::vector<VariablePtr>, int64_t>(),
             py::arg("coeffs"), py::arg("vars"), py::arg("bound"));

    py::class_<IntLinNeConstraint, Constraint, std::shared_ptr<IntLinNeConstraint>>(
            m, "IntLinNeConstraint")
        .def(py::init<std::vector<int64_t>, std::vector<VariablePtr>, int64_t>(),
             py::arg("coeffs"), py::arg("vars"), py::arg("target"));

    // ---- Reified linear constraints ----
    py::class_<IntLinEqReifConstraint, Constraint, std::shared_ptr<IntLinEqReifConstraint>>(
            m, "IntLinEqReifConstraint")
        .def(py::init<std::vector<int64_t>, std::vector<VariablePtr>, int64_t, VariablePtr>(),
             py::arg("coeffs"), py::arg("vars"), py::arg("target"), py::arg("b"));

    py::class_<IntLinNeReifConstraint, Constraint, std::shared_ptr<IntLinNeReifConstraint>>(
            m, "IntLinNeReifConstraint")
        .def(py::init<std::vector<int64_t>, std::vector<VariablePtr>, int64_t, VariablePtr>(),
             py::arg("coeffs"), py::arg("vars"), py::arg("target"), py::arg("b"));

    py::class_<IntLinLeReifConstraint, Constraint, std::shared_ptr<IntLinLeReifConstraint>>(
            m, "IntLinLeReifConstraint")
        .def(py::init<std::vector<int64_t>, std::vector<VariablePtr>, int64_t, VariablePtr>(),
             py::arg("coeffs"), py::arg("vars"), py::arg("bound"), py::arg("b"));

    py::class_<IntLinLeImpConstraint, Constraint, std::shared_ptr<IntLinLeImpConstraint>>(
            m, "IntLinLeImpConstraint")
        .def(py::init<std::vector<int64_t>, std::vector<VariablePtr>, int64_t, VariablePtr>(),
             py::arg("coeffs"), py::arg("vars"), py::arg("bound"), py::arg("b"));

    // ---- Global constraints ----
    py::class_<AllDifferentConstraint, Constraint, std::shared_ptr<AllDifferentConstraint>>(
            m, "AllDifferentConstraint")
        .def(py::init<std::vector<VariablePtr>>(), py::arg("vars"));

    py::class_<AllDifferentGACConstraint, AllDifferentConstraint,
               std::shared_ptr<AllDifferentGACConstraint>>(
            m, "AllDifferentGACConstraint")
        .def(py::init<std::vector<VariablePtr>>(), py::arg("vars"));

    py::class_<AllDifferentExcept0Constraint, Constraint,
               std::shared_ptr<AllDifferentExcept0Constraint>>(
            m, "AllDifferentExcept0Constraint")
        .def(py::init<std::vector<VariablePtr>>(), py::arg("vars"));

    py::class_<CircuitConstraint, Constraint, std::shared_ptr<CircuitConstraint>>(
            m, "CircuitConstraint")
        .def(py::init<std::vector<VariablePtr>>(), py::arg("vars"));

    py::class_<IntElementConstraint, Constraint, std::shared_ptr<IntElementConstraint>>(
            m, "IntElementConstraint")
        .def(py::init<VariablePtr, std::vector<Domain::value_type>, VariablePtr, bool>(),
             py::arg("index"), py::arg("array"), py::arg("result"),
             py::arg("zero_based") = false);

    py::class_<IntElementMonotonicConstraint, Constraint,
               std::shared_ptr<IntElementMonotonicConstraint>>(
            m, "IntElementMonotonicConstraint")
        .def(py::init<VariablePtr, std::vector<Domain::value_type>, VariablePtr,
                       IntElementMonotonicConstraint::Monotonicity, bool>(),
             py::arg("index"), py::arg("array"), py::arg("result"),
             py::arg("monotonicity"), py::arg("zero_based") = false);

    py::enum_<IntElementMonotonicConstraint::Monotonicity>(
            m, "Monotonicity")
        .value("NON_DECREASING",
               IntElementMonotonicConstraint::Monotonicity::NON_DECREASING)
        .value("NON_INCREASING",
               IntElementMonotonicConstraint::Monotonicity::NON_INCREASING);

    py::class_<ArrayVarIntElementConstraint, Constraint,
               std::shared_ptr<ArrayVarIntElementConstraint>>(
            m, "ArrayVarIntElementConstraint")
        .def(py::init<VariablePtr, std::vector<VariablePtr>, VariablePtr, bool>(),
             py::arg("index"), py::arg("array"), py::arg("result"),
             py::arg("zero_based") = false);

    py::class_<ArrayIntMaximumConstraint, Constraint,
               std::shared_ptr<ArrayIntMaximumConstraint>>(
            m, "ArrayIntMaximumConstraint")
        .def(py::init<VariablePtr, std::vector<VariablePtr>>(),
             py::arg("m"), py::arg("vars"));

    py::class_<ArrayIntMinimumConstraint, Constraint,
               std::shared_ptr<ArrayIntMinimumConstraint>>(
            m, "ArrayIntMinimumConstraint")
        .def(py::init<VariablePtr, std::vector<VariablePtr>>(),
             py::arg("m"), py::arg("vars"));

    py::class_<TableConstraint, Constraint, std::shared_ptr<TableConstraint>>(
            m, "TableConstraint")
        .def(py::init<std::vector<VariablePtr>, std::vector<Domain::value_type>>(),
             py::arg("vars"), py::arg("flat_tuples"));

    py::class_<CountEqConstraint, Constraint, std::shared_ptr<CountEqConstraint>>(
            m, "CountEqConstraint")
        .def(py::init<std::vector<VariablePtr>, Domain::value_type, VariablePtr>(),
             py::arg("x_vars"), py::arg("target"), py::arg("count_var"));

    py::class_<CountEqVarTargetConstraint, Constraint,
               std::shared_ptr<CountEqVarTargetConstraint>>(
            m, "CountEqVarTargetConstraint")
        .def(py::init<std::vector<VariablePtr>, VariablePtr, VariablePtr>(),
             py::arg("x_vars"), py::arg("y_var"), py::arg("count_var"));

    py::class_<DisjunctiveConstraint, Constraint, std::shared_ptr<DisjunctiveConstraint>>(
            m, "DisjunctiveConstraint")
        .def(py::init<std::vector<VariablePtr>, std::vector<VariablePtr>, bool>(),
             py::arg("starts"), py::arg("durations"), py::arg("strict") = true);

    py::class_<DiffnConstraint, Constraint, std::shared_ptr<DiffnConstraint>>(
            m, "DiffnConstraint")
        .def(py::init<std::vector<VariablePtr>, std::vector<VariablePtr>,
                       std::vector<VariablePtr>, std::vector<VariablePtr>, bool>(),
             py::arg("x"), py::arg("y"), py::arg("dx"), py::arg("dy"),
             py::arg("strict") = true);

    py::class_<CumulativeConstraint, Constraint, std::shared_ptr<CumulativeConstraint>>(
            m, "CumulativeConstraint")
        .def(py::init<std::vector<VariablePtr>, std::vector<VariablePtr>,
                       std::vector<VariablePtr>, VariablePtr>(),
             py::arg("starts"), py::arg("durations"),
             py::arg("requirements"), py::arg("capacity"));

    py::class_<InverseConstraint, Constraint, std::shared_ptr<InverseConstraint>>(
            m, "InverseConstraint")
        .def(py::init<std::vector<VariablePtr>, std::vector<VariablePtr>>(),
             py::arg("f"), py::arg("invf"));

    py::class_<RegularConstraint, Constraint, std::shared_ptr<RegularConstraint>>(
            m, "RegularConstraint")
        .def(py::init<std::vector<VariablePtr>, int, int, std::vector<int>, int, std::vector<int>>(),
             py::arg("vars"), py::arg("num_states"), py::arg("num_symbols"),
             py::arg("transition_flat"), py::arg("initial_state"),
             py::arg("accepting_states"));

    py::class_<NValueConstraint, Constraint, std::shared_ptr<NValueConstraint>>(
            m, "NValueConstraint")
        .def(py::init<VariablePtr, std::vector<VariablePtr>>(),
             py::arg("n_var"), py::arg("x_vars"));

    // ---- Logical constraints ----
    py::class_<ArrayBoolAndConstraint, Constraint, std::shared_ptr<ArrayBoolAndConstraint>>(
            m, "ArrayBoolAndConstraint")
        .def(py::init<std::vector<VariablePtr>, VariablePtr>(),
             py::arg("vars"), py::arg("r"));

    py::class_<ArrayBoolOrConstraint, Constraint, std::shared_ptr<ArrayBoolOrConstraint>>(
            m, "ArrayBoolOrConstraint")
        .def(py::init<std::vector<VariablePtr>, VariablePtr>(),
             py::arg("vars"), py::arg("r"));

    py::class_<BoolClauseConstraint, Constraint, std::shared_ptr<BoolClauseConstraint>>(
            m, "BoolClauseConstraint")
        .def(py::init<std::vector<VariablePtr>, std::vector<VariablePtr>>(),
             py::arg("pos"), py::arg("neg"));

    py::class_<BoolNotConstraint, Constraint, std::shared_ptr<BoolNotConstraint>>(
            m, "BoolNotConstraint")
        .def(py::init<VariablePtr, VariablePtr>(), py::arg("a"), py::arg("b"));

    py::class_<ArrayBoolXorConstraint, Constraint, std::shared_ptr<ArrayBoolXorConstraint>>(
            m, "ArrayBoolXorConstraint")
        .def(py::init<std::vector<VariablePtr>>(), py::arg("vars"));

    py::class_<BoolXorConstraint, Constraint, std::shared_ptr<BoolXorConstraint>>(
            m, "BoolXorConstraint")
        .def(py::init<VariablePtr, VariablePtr, VariablePtr>(),
             py::arg("a"), py::arg("b"), py::arg("c"));
}
