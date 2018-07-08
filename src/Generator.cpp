
#include <cmath>
#include <fstream>
#include <set>
#include <tuple>

#include "llvm/Support/raw_os_ostream.h"  /// for LLVM and STL I/O bridge
#include "llvm/Support/YAMLTraits.h"      /// for YAML underpinnings
#include "runtime/HalideRuntime.h"        /// for halide_type_code_t

#include "Generator.h"
#include "Outputs.h"
#include "Simplify.h"

namespace Halide {
namespace Internal {

/// Forward-declare Halide::Internal::YamlEmitter, for use
/// in both the forward-declaration of its corresponding
/// llvm::yaml::MappingTraits<T> specialization, and so that
/// it may be declared as a YAML sequence vector (q.v the
/// macro LLVM_YAML_IS_SEQUENCE_VECTOR() invocation sub.)
class YamlEmitter;

} /// namespace Internal
} /// namespace Halide

namespace llvm {
namespace yaml {

/// Make the internal GeneratorParam, GeneratorInput,
/// and GeneratorOutput base types, as well as pointers
/// to same, available within the llvm::yaml namespace:

using  param_t = Halide::Internal::GeneratorParamBase;
using  input_t = Halide::Internal::GeneratorInputBase;
using output_t = Halide::Internal::GeneratorOutputBase;

using  param_ptr_t = std::add_pointer_t<param_t>;
using  input_ptr_t = std::add_pointer_t<input_t>;
using output_ptr_t = std::add_pointer_t<output_t>;

/// Forward-declare the MappingTraits specialization
/// for Halide::Internal::YamlEmitter, such that this
/// specialization may be declared as a friendly member
/// of the Halide::Internal::YamlEmitter class itself:

template <>
struct MappingTraits<Halide::Internal::YamlEmitter const>;

} /// namespace yaml
} /// namespace llvm

/// Inform the LLVM YAML system that a std::vector containing
/// pointers to instances of our GeneratorXXX base types are
/// to be considered “sequence vectors”; q.v. “Utility Macros”
/// (¶ https://llvm.org/docs/YamlIO.html#utility-macros sub.)
/// in the LLVM YAML I/O informal information brochure.

LLVM_YAML_IS_SEQUENCE_VECTOR(param_ptr_t);
LLVM_YAML_IS_SEQUENCE_VECTOR(input_ptr_t);
LLVM_YAML_IS_SEQUENCE_VECTOR(output_ptr_t);

/// We can also have a sequence of Halide Types:

LLVM_YAML_IS_SEQUENCE_VECTOR(Halide::Type);

/// … and of YamlEmitters (which act as the top-level class
/// capable of YAML output serialization in our traited heiarchy):

LLVM_YAML_IS_SEQUENCE_VECTOR(Halide::Internal::YamlEmitter const);

namespace Halide {

GeneratorContext::GeneratorContext(const Target &t, bool auto_schedule,
                                   const MachineParams &machine_params)
    : target("target", t),
      auto_schedule("auto_schedule", auto_schedule),
      machine_params("machine_params", machine_params),
      externs_map(std::make_shared<ExternsMap>()),
      value_tracker(std::make_shared<Internal::ValueTracker>()) {}

GeneratorContext::~GeneratorContext() {
    // nothing
}

void GeneratorContext::init_from_context(const Halide::GeneratorContext &context) {
    target.set(context.get_target());
    auto_schedule.set(context.get_auto_schedule());
    machine_params.set(context.get_machine_params());
    value_tracker = context.get_value_tracker();
    externs_map = context.get_externs_map();
}

namespace Internal {

namespace {

// Return true iff the name is valid for Generators or Params.
// (NOTE: gcc didn't add proper std::regex support until v4.9;
// we don't yet require this, hence the hand-rolled replacement.)

bool is_alpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

// Note that this includes '_'
bool is_alnum(char c) { return is_alpha(c) || (c == '_') || (c >= '0' && c <= '9'); }

// Basically, a valid C identifier, except:
//
// -- initial _ is forbidden (rather than merely "reserved")
// -- two underscores in a row is also forbidden
bool is_valid_name(const std::string& n) {
    if (n.empty()) return false;
    if (!is_alpha(n[0])) return false;
    for (size_t i = 1; i < n.size(); ++i) {
        if (!is_alnum(n[i])) return false;
        if (n[i] == '_' && n[i-1] == '_') return false;
    }
    return true;
}

std::string compute_base_path(const std::string &output_dir,
                              const std::string &function_name,
                              const std::string &file_base_name) {
    std::vector<std::string> namespaces;
    std::string simple_name = extract_namespaces(function_name, namespaces);
    std::string base_path = output_dir + "/" + (file_base_name.empty() ? simple_name : file_base_name);
    return base_path;
}

std::string get_extension(const std::string& def, const GeneratorBase::EmitOptions &options) {
    auto it = options.substitutions.find(def);
    if (it != options.substitutions.end()) {
        return it->second;
    }
    return def;
}

Outputs compute_outputs(const Target &target,
                        const std::string &base_path,
                        const GeneratorBase::EmitOptions &options) {
    const bool is_windows_coff = target.os == Target::Windows &&
                                !target.has_feature(Target::MinGW);
    Outputs output_files;
    if (options.emit_o) {
        if (is_windows_coff) {
            // If it's windows, then we're emitting a COFF file
            output_files.object_name = base_path + get_extension(".obj", options);
        } else {
            // Otherwise it is an ELF or Mach-o
            output_files.object_name = base_path + get_extension(".o", options);
        }
    }
    if (options.emit_assembly) {
        output_files.assembly_name = base_path + get_extension(".s", options);
    }
    if (options.emit_bitcode) {
        // In this case, bitcode refers to the LLVM IR generated by Halide
        // and passed to LLVM.
        output_files.bitcode_name = base_path + get_extension(".bc", options);
    }
    if (options.emit_h) {
        output_files.c_header_name = base_path + get_extension(".h", options);
    }
    if (options.emit_cpp) {
        output_files.c_source_name = base_path + get_extension(".cpp", options);
    }
    if (options.emit_python_extension) {
        output_files.python_extension_name = base_path + get_extension(".py.c", options);
    }
    if (options.emit_stmt) {
        output_files.stmt_name = base_path + get_extension(".stmt", options);
    }
    if (options.emit_stmt_html) {
        output_files.stmt_html_name = base_path + get_extension(".html", options);
    }
    if (options.emit_static_library) {
        if (is_windows_coff) {
            output_files.static_library_name = base_path + get_extension(".lib", options);
        } else {
            output_files.static_library_name = base_path + get_extension(".a", options);
        }
    }
    if (options.emit_schedule) {
        output_files.schedule_name = base_path + get_extension(".schedule", options);
    }
    if (options.emit_yaml) {
        output_files.yaml_name = base_path + get_extension(".yaml", options);
    }
    return output_files;
}

Argument to_argument(const Internal::Parameter &param) {
    Expr def, min, max;
    if (!param.is_buffer()) {
        def = param.scalar_expr();
        min = param.min_value();
        max = param.max_value();
    }
    return Argument(param.name(),
        param.is_buffer() ? Argument::InputBuffer : Argument::InputScalar,
        param.type(), param.dimensions(), def, min, max);
}

Func make_param_func(const Parameter &p, const std::string &name) {
    internal_assert(p.is_buffer());
    Func f(name + "_im");
    auto b =  p.buffer();
    if (b.defined()) {
        // If the Parameter has an explicit BufferPtr set, bind directly to it
        f(_) = b(_);
    } else {
        std::vector<Var> args;
        std::vector<Expr> args_expr;
        for (int i = 0; i < p.dimensions(); ++i) {
            Var v = Var::implicit(i);
            args.push_back(v);
            args_expr.push_back(v);
        }
        f(args) = Internal::Call::make(p, args_expr);
    }
    return f;
}

}  // namespace

std::vector<Type> parse_halide_type_list(const std::string &types) {
    const auto &e = get_halide_type_enum_map();
    std::vector<Type> result;
    for (auto t : split_string(types, ",")) {
        auto it = e.find(t);
        user_assert(it != e.end()) << "Type not found: " << t;
        result.push_back(it->second);
    }
    return result;
}

void ValueTracker::track_values(const std::string &name, const std::vector<Expr> &values) {
    std::vector<std::vector<Expr>> &history = values_history[name];
    if (history.empty()) {
        for (size_t i = 0; i < values.size(); ++i) {
            history.push_back({values[i]});
        }
        return;
    }

    internal_assert(history.size() == values.size())
        << "Expected values of size " << history.size()
        << " but saw size " << values.size()
        << " for name " << name << "\n";

    // For each item, see if we have a new unique value
    for (size_t i = 0; i < values.size(); ++i) {
        Expr oldval = history[i].back();
        Expr newval = values[i];
        if (oldval.defined() && newval.defined()) {
            if (can_prove(newval == oldval)) {
                continue;
            }
        } else if (!oldval.defined() && !newval.defined()) {
            // Expr::operator== doesn't work with undefined
            // values, but they are equal for our purposes here.
            continue;
        }
        history[i].push_back(newval);
        // If we exceed max_unique_values, fail immediately.
        // TODO: could be useful to log all the entries that
        // overflow max_unique_values before failing.
        // TODO: this could be more helpful about labeling the values
        // that have multiple setttings.
        if (history[i].size() > max_unique_values) {
            std::ostringstream o;
            o << "Saw too many unique values in ValueTracker[" + std::to_string(i) + "]; "
              << "expected a maximum of " << max_unique_values << ":\n";
            for (auto e : history[i]) {
                o << "    " << e << "\n";
            }
            user_error << o.str();
        }
    }
}

std::vector<Expr> parameter_constraints(const Parameter &p) {
    internal_assert(p.defined());
    std::vector<Expr> values;
    values.push_back(Expr(p.host_alignment()));
    if (p.is_buffer()) {
        for (int i = 0; i < p.dimensions(); ++i) {
            values.push_back(p.min_constraint(i));
            values.push_back(p.extent_constraint(i));
            values.push_back(p.stride_constraint(i));
        }
    } else {
        values.push_back(p.min_value());
        values.push_back(p.max_value());
    }
    return values;
}

class EmitterBase {
    
    /// Abstract base class for an emitter -- a class that describes generator
    /// output for a simple mode, e.g. C++ stubs or YAML metadata (q.v. concrete
    /// subclass definitions sub.) -- one that does not require the construction
    /// of a Module object in order to emit code or data.
    
    public:
        struct InputInfo {
            std::string c_type;
            std::string name;
        };
        
    public:
        struct OutputInfo {
            std::string name;
            std::string ctype;
            std::string getter;
        };
    
    public:
        using      stringvec_t = std::vector<std::string>;
        using  param_ptr_vec_t = std::vector<std::add_pointer_t<Internal::GeneratorParamBase>>;
        using  input_ptr_vec_t = std::vector<std::add_pointer_t<Internal::GeneratorInputBase>>;
        using output_ptr_vec_t = std::vector<std::add_pointer_t<Internal::GeneratorOutputBase>>;
        using        typevec_t = std::vector<Halide::Type>;
        using     in_infovec_t = std::vector<InputInfo>;
        using    out_infovec_t = std::vector<OutputInfo>;
        using       out_info_t = std::tuple<out_infovec_t, bool>;
    
    public:
        EmitterBase(std::ostream& dest,
                    std::string const& generator_registered_name,
                    std::string const& generator_stub_name,
                    param_ptr_vec_t const& generator_params,
                    input_ptr_vec_t const& inputs,
                    output_ptr_vec_t const& outputs)
            : stream(dest),
              generator_registered_name(generator_registered_name),
              generator_stub_name(generator_stub_name),
              generator_params(select_generator_params(generator_params)),
              inputs(inputs), outputs(outputs) {
           namespaces = split_string(generator_stub_name, "::");
           internal_assert(namespaces.size() >= 1);
           if (namespaces[0].empty()) {
               /// We have a name like ::foo::bar::baz; omit the first empty ns.
               namespaces.erase(namespaces.begin());
               internal_assert(namespaces.size() >= 2);
           }
           class_name = namespaces.back();
           namespaces.pop_back();
        }
        
    public:
        /// The EmitterBase class is an ABC:
        virtual void emit() const = 0;
        virtual ~EmitterBase() {}
        
    protected:
        std::ostream& stream;
        const std::string generator_registered_name;
        const std::string generator_stub_name;
        std::string class_name;
        stringvec_t namespaces;
        const param_ptr_vec_t generator_params;
        const input_ptr_vec_t inputs;
        const output_ptr_vec_t outputs;
        mutable int indent_level{ 0 };
        
    protected:
        param_ptr_vec_t select_generator_params(param_ptr_vec_t const& in) const {
            param_ptr_vec_t out;
            for (auto p : in) {
                /// These are always propagated specially.
                if (p->name == "target" ||
                    p->name == "auto_schedule" ||
                    p->name == "machine_params") continue;
                if (p->is_synthetic_param()) continue;
                out.push_back(p);
            }
            return out;
        }
        
        /// Extract relevant info from the vector of Input pointers, copying
        /// their data into a vector of EmitterBase::InputInfo structs:
        
        in_infovec_t get_input_info() const {
            in_infovec_t outvec;
            outvec.reserve(inputs.size());
            for (auto input : inputs) {
                std::string c_type = input->get_c_type();
                if (input->is_array()) {
                    c_type = "std::vector<" + c_type + ">";
                }
                outvec.push_back({ c_type, input->name() });
            }
            return outvec;
        }
        
        /// Extract relevant info from the vector of Output pointers, copying
        /// their data into a vector of EmitterBase::OutputInfo structs and
        /// returning this in a std::tuple, alongside a boolean value indicating
        /// whether or not all of the outputs are of type Halide::Func:
        
        out_info_t get_output_info() const {
            bool all_outputs_are_func = true;
            out_infovec_t outvec;
            outvec.reserve(outputs.size());
            for (auto output : outputs) {
                std::string c_type = output->get_c_type();
                std::string getter;
                const bool is_func = (c_type == "Func");
                if (output->is_array()) {
                    getter = is_func ? "get_array_output" : "get_array_output_buffer";
                } else {
                    getter = is_func ?       "get_output" : "get_output_buffer";
                }
                if (!is_func) { getter += "<" + c_type + ">"; }
                outvec.push_back({ output->name(),
                                   output->is_array() ? "std::vector<" + c_type + ">" : c_type,
                  getter + "(\"" + output->name() + "\")" });
                all_outputs_are_func = all_outputs_are_func && is_func;
            }
            return { outvec, all_outputs_are_func };
        }
        
    protected:
        /// Emit spaces according to the current indentation level:
        std::string indent() const;
};

class StubEmitter : public EmitterBase {
    
    public:
        using Super = EmitterBase;
        using Super::param_ptr_vec_t;
        using Super::input_ptr_vec_t;
        using Super::output_ptr_vec_t;
    
    public:
        virtual void emit() const override;
        StubEmitter(std::ostream& dest,
                    std::string const& generator_registered_name,
                    std::string const& generator_stub_name,
                    param_ptr_vec_t const& generator_params,
                    input_ptr_vec_t const& inputs,
                    output_ptr_vec_t const& outputs)
            : Super(dest,
                    generator_registered_name,
                    generator_stub_name,
                    generator_params,
                    inputs, outputs) {}
    
    private:
        void emit_inputs_struct() const;
        void emit_generator_params_struct() const;
    
};

class YamlEmitter : public EmitterBase {
    
    public:
        friend struct llvm::yaml::MappingTraits<YamlEmitter const>;
    
    public:
        using Super = EmitterBase;
        using Super::param_ptr_vec_t;
        using Super::input_ptr_vec_t;
        using Super::output_ptr_vec_t;
    
    private:
        using ostream_t = llvm::raw_os_ostream;
        using youtput_t = llvm::yaml::Output;
    
    private:
        static constexpr std::size_t default_column_width = 80;
    
    public:
        virtual void emit() const override;
        YamlEmitter(std::ostream& dest,
                    std::string const& generator_registered_name,
                    std::string const& generator_stub_name,
                    param_ptr_vec_t const& generator_params,
                    input_ptr_vec_t const& inputs,
                    output_ptr_vec_t const& outputs,
                    std::size_t column_width = default_column_width)
            : Super(dest,
                    generator_registered_name,
                    generator_stub_name,
                    generator_params,
                    inputs, outputs),
              llostream(stream),
              youtput(llostream, nullptr, column_width) {}
    
    private:
        ostream_t llostream;
        mutable youtput_t youtput;
    
};

} /// namespace Internal
} /// namespace Halide

/// Inform the YAML system that we would like to serialize
/// std::vectors of the embedded InputInfo and OutputInfo
/// structs (q.v. their respective declarations within the
/// Halide::Internal::EmitterBase class, supra.):

LLVM_YAML_IS_FLOW_SEQUENCE_VECTOR(Halide::Internal::EmitterBase::InputInfo);
LLVM_YAML_IS_FLOW_SEQUENCE_VECTOR(Halide::Internal::EmitterBase::OutputInfo);

namespace llvm {
namespace yaml {

/// These specializations of templated types in the llvm::yaml namespace
/// allow for the serialized I/O of our types -- those upon which we are
/// specializing the llvm::yaml types -- to and from YAML. In this case,
/// most of our specializations make our given types only workable within
/// the LLVM YAML framework for output only. Input can’t work in our cases
/// as the llvm::yaml::IO::mapRequired(…) templated member function attempts
/// to cover both the read and write cases; in the case of nearly all of
/// the Halide types, the data we want to serialize is obtained from one or
/// more member function calls, or some other manner.

/// Because llvm::yaml::IO::mapRequired(…) requires a non-const T& type to
/// be passed as its second argument, we have to use a non-const intermediate
/// variable to store the values we obtain from our types’ member functions;
/// when serializing output, llvm::yaml can read from these variables without
/// issue, but writing deserialized input values to them would be meaningless.
/// That is why most of the specializations are described as “output-only” in
/// the comments below. The few types whose members could be used for reading
/// and writing both, as llvm::yaml expects, are described as "I/O" – this
/// despite the fact that deserialization from YAML itself is really kind of a
/// nonsensical operation: at the time of writing, the goal of YAML output is
/// to furnish a generators’ metadata to a consuming program, and not to render
/// the generator code itself in YAML.

template <>
struct ScalarEnumerationTraits<halide_type_code_t> {
    /// Traited YAML I/O serialization for the halide_type_code_t enum type,
    /// as declared in HalideRuntime.h:
    
    static void enumeration(IO& io, halide_type_code_t& typecode) {
        io.enumCase(typecode,   "halide_type_int",      halide_type_int);
        io.enumCase(typecode,   "halide_type_uint",     halide_type_uint);
        io.enumCase(typecode,   "halide_type_float",    halide_type_float);
        io.enumCase(typecode,   "halide_type_handle",   halide_type_handle);
    }
};

template <>
struct MappingTraits<Halide::Type> {
    /// Traited YAML output-only serialization for the Halide::Type class,
    /// as declared in Type.h:
    
    static const bool flow = true; /// print values inline
    static void mapping(IO& io, Halide::Type& haltype) {
                           int bits         = haltype.bits();
                          int lanes         = haltype.lanes();
        halide_type_code_t typecode         = haltype.code();
                 std::string c_type         = Halide::Internal::halide_type_to_c_type(haltype);
               std::string c_source         = Halide::Internal::halide_type_to_c_source(haltype);
        
        io.mapRequired("name",                c_source);
        io.mapRequired("bits",                bits);
        io.mapRequired("lanes",               lanes);
        io.mapRequired("typecode",            typecode);
        io.mapRequired("c-type",              c_type);
    }
};

template <>
struct MappingTraits<param_ptr_t> {
    /// Traited YAML output-only serialization for Halide::Internal::GeneratorParamBase
    /// pointer types, as declared in Generator.h:
    
    static const std::string default_call_to_string;
    
    static void mapping(IO& io,  param_ptr_t& param) {
        std::string name                    = param->name;
        std::string default_value           = param->get_default_value();
        std::string c_type                  = param->get_c_type();
        std::string type_decls              = param->get_type_decls();
               bool is_synthetic            = param->is_synthetic_param();
               bool is_looplevel            = param->is_looplevel_param();
        std::string call_to_string          = is_looplevel ?  default_call_to_string
                                                           :   param->call_to_string(param->name);
        
        io.mapRequired("name",                name);
        io.mapRequired("default",             default_value);
        io.mapRequired("c-type",              c_type);
        io.mapRequired("type-decls",          type_decls);
        io.mapRequired("is-synthetic",        is_synthetic);
        io.mapRequired("is-looplevel",        is_looplevel);
        io.mapOptional("call-to-string",      call_to_string, default_call_to_string);
    }
};

/// If the value passed to llvm::yaml::IO::mapOptional(…) compares equal (using operator==())
/// with the default value passed -- the third argument -- the value is not included in the
/// emitted YAML mapping. We use a static-const empty string as this default value, allowing
/// us to bypass emitting “call_to_string” for instances of GeneratorParam<LoopLevel>:

std::string const MappingTraits<param_ptr_t>::default_call_to_string = std::string{ "" };

/// The Halide::Internal::IOKind enum class type is the return type from the member function
/// Halide::Internal::GIOBase::kind():

using Halide::Internal::IOKind;

template <>
struct ScalarEnumerationTraits<Halide::Internal::IOKind> {
    /// Traited YAML I/O serialization for the Halide::Internal::IOKind enum type
    /// (the return type of GIOBase::kind()) as declared in Generator.h:
    
    static void enumeration(IO& io, Halide::Internal::IOKind& iokind) {
        io.enumCase(iokind,     "IOKind::Scalar",     IOKind::Scalar);
        io.enumCase(iokind,     "IOKind::Function",   IOKind::Function);
        io.enumCase(iokind,     "IOKind::Buffer",     IOKind::Buffer);
    }
};

/// Both Halide::Internal::GeneratorInputBase and Halide::Internal::GeneratorOutputBase
/// inherit from Halide::Internal::GIOBase, which gives them a “types()” method, returning
/// a std::vector<Halide::Type> we would like to serialize:

using typevec_t = Halide::Internal::EmitterBase::typevec_t;

template <>
struct MappingTraits<input_ptr_t> {
    /// Traited YAML output-only serialization for Halide::Internal::GeneratorInputBase
    /// pointer types, as declared in Generator.h:
    
    static const int       default_array_size;
    static const int       default_dims;
    static const typevec_t default_types;
    
    static void mapping(IO& io, input_ptr_t& input) {
        std::string name                   = input->name();
        std::string c_type                 = input->get_c_type();
             IOKind kind                   = input->kind();
               bool array_size_defined     = input->array_size_defined();
               bool dims_defined           = input->dims_defined();
               bool types_defined          = input->types_defined();
                int array_size             = array_size_defined ? static_cast<int>(
                                                                   input->array_size())
                                                                : default_array_size;
                int dims                   =       dims_defined ?  input->dims()
                                                                : default_dims;
          typevec_t types                  =      types_defined ?  input->types()
                                                                : default_types;
        
        io.mapRequired("name",               name);
        io.mapRequired("c-type",             c_type);
        io.mapRequired("io-kind",            kind);
        io.mapOptional("rank",               array_size,          default_array_size);
        io.mapOptional("dimensions",         dims,                default_dims);
        io.mapOptional("types",              types,               default_types);
    }
};

      int const MappingTraits<input_ptr_t>::default_array_size = int{ 1 };
      int const MappingTraits<input_ptr_t>::default_dims       = int{ 0 };
typevec_t const MappingTraits<input_ptr_t>::default_types      = typevec_t{};

template <>
struct MappingTraits<output_ptr_t> {
    /// Traited YAML output-only serialization for Halide::Internal::GeneratorOutputBase
    /// pointer types, as declared in Generator.h:
    
    static const int       default_array_size;
    static const int       default_dims;
    static const typevec_t default_types;
    
    static void mapping(IO& io, output_ptr_t& output) {
        std::string name                    = output->name();
        std::string c_type                  = output->get_c_type();
             IOKind kind                    = output->kind();
               bool array_size_defined      = output->array_size_defined();
               bool dims_defined            = output->dims_defined();
               bool types_defined           = output->types_defined();
                int array_size              = array_size_defined ? static_cast<int>(
                                                                   output->array_size())
                                                                 : default_array_size;
                int dims                    =       dims_defined ? output->dims()
                                                                 : default_dims;
          typevec_t types                   =      types_defined ? output->types()
                                                                 : default_types;
        
        io.mapRequired("name",                name);
        io.mapRequired("c-type",              c_type);
        io.mapRequired("io-kind",             kind);
        io.mapOptional("rank",                array_size,          default_array_size);
        io.mapOptional("dimensions",          dims,                default_dims);
        io.mapOptional("types",               types,               default_types);
    }
};

      int const MappingTraits<output_ptr_t>::default_array_size = int{ 1 };
      int const MappingTraits<output_ptr_t>::default_dims       = int{ 0 };
typevec_t const MappingTraits<output_ptr_t>::default_types      = typevec_t{};

using InputInfo = Halide::Internal::EmitterBase::InputInfo;

template <>
struct MappingTraits<InputInfo> {
    /// Traited YAML I/O serialization for Halide::Internal::EmitterBase::InputInfo
    /// as declared in Generator.cpp (q.v. supra.):
    
    static const bool flow = true; /// print values inline
    static void mapping(IO& io, InputInfo& input_info) {
        io.mapRequired("name",             input_info.name);
        io.mapRequired("c-type",           input_info.c_type);
    }
};

using OutputInfo = Halide::Internal::EmitterBase::OutputInfo;

template <>
struct MappingTraits<OutputInfo> {
    /// Traited YAML I/O serialization for Halide::Internal::EmitterBase::OutputInfo
    /// as declared in Generator.cpp (q.v. supra.):
    
    static const bool flow = true; /// print values inline
    static void mapping(IO& io, OutputInfo& output_info) {
        io.mapRequired("name",              output_info.name);
        io.mapRequired("c-type",            output_info.ctype);
        io.mapRequired("getter",            output_info.getter);
    }
};

using                     Halide::Internal::YamlEmitter;
using stringvec_t       = Halide::Internal::EmitterBase::stringvec_t;
using param_ptr_vec_t   = Halide::Internal::EmitterBase::param_ptr_vec_t;
using input_ptr_vec_t   = Halide::Internal::EmitterBase::input_ptr_vec_t;
using output_ptr_vec_t  = Halide::Internal::EmitterBase::output_ptr_vec_t;
using in_infovec_t      = Halide::Internal::EmitterBase::in_infovec_t;
using out_infovec_t     = Halide::Internal::EmitterBase::out_infovec_t;

template <>
struct MappingTraits<YamlEmitter const> {
    /// Traited YAML output-only serialization for the Halide::Internal::YamlEmitter
    /// class -- an instance of which corresponds one-to-one with the actual generator
    /// whose information we would like to serialize -- as declared in Generator.cpp
    /// (q.v. supra.):
    
    static void mapping(IO& io, YamlEmitter const& yammitter) {
             std::string name                    = yammitter.generator_registered_name;
             std::string stub_name               = yammitter.generator_stub_name;
             std::string class_name              = yammitter.class_name;
             stringvec_t namespaces              = yammitter.namespaces;
         param_ptr_vec_t params                  = yammitter.generator_params;
         input_ptr_vec_t inputs                  = yammitter.inputs;
        output_ptr_vec_t outputs                 = yammitter.outputs;
            in_infovec_t input_info              = yammitter.get_input_info();
           out_infovec_t output_info;
                    bool outputs_all_funcs{ true };
        std::tie(output_info, outputs_all_funcs) = yammitter.get_output_info();
        
        io.mapRequired("name",                     name);
        io.mapRequired("stub-name",                stub_name);
        io.mapRequired("class-name",               class_name);
        io.mapRequired("namespaces",               namespaces);
        io.mapRequired("params",                   params);
        io.mapRequired("inputs",                   inputs);
        io.mapRequired("outputs",                  outputs);
        io.mapRequired("outputs-all-funcs",        outputs_all_funcs);
        io.mapRequired("input-info",               input_info);
        io.mapRequired("output-info",              output_info);
    }
};

} /// namespace yaml
} /// namespace llvm

namespace Halide {
namespace Internal {

/// All that is needed to emit YAML for a given generator is to
/// call the operator<<(…) overload of an instance of llvm::yaml::IO,
/// passing an instance of Halide::Internal::YamlEmitter that has been
/// instantiated for the generator in question -- q.v. the definition for
/// Halide::Internal::GeneratorBase::emit_yaml(…) sub. for the locus of
/// this instantiation.

/// The constructor for llvm::yaml::IO takes an instance of llvm::raw_os_ostream,
/// which itself wraps a std::ostream instance; q.v. the constructor definition
/// for Halide::Internal::YamlEmitter supra. for how these instances are managed.

/// Hence, the single line that defines Halide::Internal::YamlEmitter::emit(),
/// which begets a chain of internal llvm::yaml calls, all of which eventually
/// calling on the static member functions of our specialized YAML trait structs,
/// q.v. supra.:

void YamlEmitter::emit() const {
    youtput << *this;
}

std::string EmitterBase::indent() const {
    return std::string(
        static_cast<std::string::size_type>(
            indent_level * 2), ' ');
}

void StubEmitter::emit_generator_params_struct() const {
    auto const& v = generator_params;
    std::string name = "GeneratorParams";
    stream << indent() << "struct " << name << " final {\n";
    indent_level++;
    if (!v.empty()) {
        for (auto p : v) {
            stream << indent() << p->get_c_type() << " " << p->name << "{ " << p->get_default_value() << " };\n";
        }
        stream << "\n";
    }

    stream << indent() << name << "() {}\n";
    stream << "\n";

    if (!v.empty()) {
        stream << indent() << name << "(\n";
        indent_level++;
        std::string comma = "";
        for (auto p : v) {
            stream << indent() << comma << p->get_c_type() << " " << p->name << "\n";
            comma = ", ";
        }
        indent_level--;
        stream << indent() << ") : \n";
        indent_level++;
        comma = "";
        for (auto p : v) {
            stream << indent() << comma << p->name << "(" << p->name << ")\n";
            comma = ", ";
        }
        indent_level--;
        stream << indent() << "{\n";
        stream << indent() << "}\n";
        stream << "\n";
    }

    stream << indent() << "inline HALIDE_NO_USER_CODE_INLINE Halide::Internal::GeneratorParamsMap to_generator_params_map() const {\n";
    indent_level++;
    stream << indent() << "return {\n";
    indent_level++;
    std::string comma = "";
    for (auto p : v) {
        stream << indent() << comma << "{\"" << p->name << "\", ";
        if (p->is_looplevel_param()) {
            stream << p->name << "}\n";
        } else {
            stream << p->call_to_string(p->name) << "}\n";
        }
        comma = ", ";
    }
    indent_level--;
    stream << indent() << "};\n";
    indent_level--;
    stream << indent() << "}\n";

    indent_level--;
    stream << indent() << "};\n";
    stream << "\n";
}

void StubEmitter::emit_inputs_struct() const {
    /// Load up a std::vector<InputInfo> from our GeneratorInputs:
    in_infovec_t in_info = get_input_info();
    
    const std::string name = "Inputs";
    stream << indent() << "struct " << name << " final {\n";
    indent_level++;
    for (auto in : in_info) {
        stream << indent() << in.c_type << " " << in.name << ";\n";
    }
    stream << "\n";

    stream << indent() << name << "() {}\n";
    stream << "\n";

    stream << indent() << name << "(\n";
    indent_level++;
    std::string comma = "";
    for (auto in : in_info) {
        stream << indent() << comma << "const " << in.c_type << "& " << in.name << "\n";
        comma = ", ";
    }
    indent_level--;
    stream << indent() << ") : \n";
    indent_level++;
    comma = "";
    for (auto in : in_info) {
        stream << indent() << comma << in.name << "(" << in.name << ")\n";
        comma = ", ";
    }
    indent_level--;
    stream << indent() << "{\n";
    stream << indent() << "}\n";

    indent_level--;
    stream << indent() << "};\n";
    stream << "\n";
}

void StubEmitter::emit() const {
    if (outputs.empty()) {
        // The generator can't support a real stub. Instead, generate an (essentially)
        // empty .stub.h file, so that build systems like Bazel will still get the output file
        // they expected. Note that we deliberately don't emit an ifndef header guard,
        // since we can't reliably assume that the generator_name will be globally unique;
        // on the other hand, since this file is just a couple of comments, it's
        // really not an issue if it's included multiple times.
        stream << "/* MACHINE-GENERATED - DO NOT EDIT */\n";
        stream << "/* The Generator named " << generator_registered_name << " uses ImageParam or Param, thus cannot have a Stub generated. */\n";
        return;
    }
    
    out_infovec_t out_info;
    bool all_outputs_are_func = true;
    
    /// Load up a std::vector<OutputInfo> from our GeneratorInputs,
    /// passing along a boolean flag indicating if all outputs are Funcs:
    std::tie(out_info, all_outputs_are_func) = get_output_info();
    
    std::ostringstream guard;
    guard << "HALIDE_STUB";
    for (auto const& ns : namespaces) {
        guard << "_" << ns;
    }
    guard << "_" << class_name;

    stream << indent() << "#ifndef " << guard.str() << "\n";
    stream << indent() << "#define " << guard.str() << "\n";
    stream << "\n";

    stream << indent() << "/* MACHINE-GENERATED - DO NOT EDIT */\n";
    stream << "\n";

    stream << indent() << "#include <cassert>\n";
    stream << indent() << "#include <map>\n";
    stream << indent() << "#include <memory>\n";
    stream << indent() << "#include <string>\n";
    stream << indent() << "#include <utility>\n";
    stream << indent() << "#include <vector>\n";
    stream << "\n";
    stream << indent() << "#include \"Halide.h\"\n";
    stream << "\n";

    stream << "namespace halide_register_generator {\n";
    stream << "namespace " << generator_registered_name << "_ns {\n";
    stream << "extern std::unique_ptr<Halide::Internal::GeneratorBase> factory(const Halide::GeneratorContext& context);\n";
    stream << "}  // namespace halide_register_generator\n";
    stream << "}  // namespace " << generator_registered_name << "\n";
    stream << "\n";

    for (auto const& ns : namespaces) {
        stream << indent() << "namespace " << ns << " {\n";
    }
    stream << "\n";

    for (auto p : generator_params) {
        std::string decl = p->get_type_decls();
        if (decl.empty()) continue;
        stream << decl << "\n";
    }

    stream << indent() << "class " << class_name << " final : public Halide::NamesInterface {\n";
    stream << indent() << "public:\n";
    indent_level++;

    emit_inputs_struct();
    emit_generator_params_struct();

    stream << indent() << "struct Outputs final {\n";
    indent_level++;
    stream << indent() << "// Outputs\n";
    for (auto const& out : out_info) {
        stream << indent() << out.ctype << " " << out.name << ";\n";
    }

    stream << "\n";
    stream << indent() << "// The Target used\n";
    stream << indent() << "Target target;\n";

    if (out_info.size() == 1) {
        stream << "\n";
        if (all_outputs_are_func) {
            std::string name = out_info.at(0).name;
            auto output = outputs[0];
            if (output->is_array()) {
                stream << indent() << "operator std::vector<Halide::Func>() const {\n";
                indent_level++;
                stream << indent() << "return " << name << ";\n";
                indent_level--;
                stream << indent() << "}\n";

                stream << indent() << "Halide::Func operator[](size_t i) const {\n";
                indent_level++;
                stream << indent() << "return " << name << "[i];\n";
                indent_level--;
                stream << indent() << "}\n";

                stream << indent() << "Halide::Func at(size_t i) const {\n";
                indent_level++;
                stream << indent() << "return " << name << ".at(i);\n";
                indent_level--;
                stream << indent() << "}\n";

                stream << indent() << "// operator operator()() overloads omitted because the sole Output is array-of-Func.\n";
            } else {
                // If there is exactly one output, add overloads
                // for operator Func and operator().
                stream << indent() << "operator Halide::Func() const {\n";
                indent_level++;
                stream << indent() << "return " << name << ";\n";
                indent_level--;
                stream << indent() << "}\n";

                stream << "\n";
                stream << indent() << "template <typename... Args>\n";
                stream << indent() << "Halide::FuncRef operator()(Args&&... args) const {\n";
                indent_level++;
                stream << indent() << "return " << name << "(std::forward<Args>(args)...);\n";
                indent_level--;
                stream << indent() << "}\n";

                stream << "\n";
                stream << indent() << "template <typename ExprOrVar>\n";
                stream << indent() << "Halide::FuncRef operator()(std::vector<ExprOrVar> args) const {\n";
                indent_level++;
                stream << indent() << "return " << name << "()(args);\n";
                indent_level--;
                stream << indent() << "}\n";
            }
        } else {
            stream << indent() << "// operator Func() and operator()() overloads omitted because the sole Output is not Func.\n";
        }
    }

    stream << "\n";
    if (all_outputs_are_func) {
        stream << indent() << "Halide::Pipeline get_pipeline() const {\n";
        indent_level++;
        stream << indent() << "return Halide::Pipeline(std::vector<Halide::Func>{\n";
        indent_level++;
        int commas = static_cast<int>(out_info.size()) - 1;
        for (auto const& out : out_info) {
            stream << indent() << out.name << (commas-- ? "," : "") << "\n";
        }
        indent_level--;
        stream << indent() << "});\n";
        indent_level--;
        stream << indent() << "}\n";

        stream << "\n";
        stream << indent() << "Halide::Realization realize(std::vector<int32_t> sizes) {\n";
        indent_level++;
        stream << indent() << "return get_pipeline().realize(sizes, target);\n";
        indent_level--;
        stream << indent() << "}\n";

        stream << "\n";
        stream << indent() << "template <typename... Args, typename std::enable_if<Halide::Internal::NoRealizations<Args...>::value>::type * = nullptr>\n";
        stream << indent() << "Halide::Realization realize(Args&&... args) {\n";
        indent_level++;
        stream << indent() << "return get_pipeline().realize(std::forward<Args>(args)..., target);\n";
        indent_level--;
        stream << indent() << "}\n";

        stream << "\n";
        stream << indent() << "void realize(Halide::Realization r) {\n";
        indent_level++;
        stream << indent() << "get_pipeline().realize(r, target);\n";
        indent_level--;
        stream << indent() << "}\n";
    } else {
        stream << indent() << "// get_pipeline() and realize() overloads omitted because some Outputs are not Func.\n";
    }

    indent_level--;
    stream << indent() << "};\n";
    stream << "\n";

    stream << indent() << "HALIDE_NO_USER_CODE_INLINE static Outputs generate(\n";
    indent_level++;
    stream << indent() << "const GeneratorContext& context,\n";
    stream << indent() << "const Inputs& inputs,\n";
    stream << indent() << "const GeneratorParams& generator_params = GeneratorParams()\n";
    indent_level--;
    stream << indent() << ")\n";
    stream << indent() << "{\n";
    indent_level++;
    stream << indent() << "using Stub = Halide::Internal::GeneratorStub;\n";
    stream << indent() << "Stub stub(\n";
    indent_level++;
    stream << indent() << "context,\n";
    stream << indent() << "halide_register_generator::" << generator_registered_name << "_ns::factory,\n";
    stream << indent() << "generator_params.to_generator_params_map(),\n";
    stream << indent() << "{\n";
    indent_level++;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        stream << indent() << "Stub::to_stub_input_vector(inputs." << inputs[i]->name() << ")";
        stream << ",\n";
    }
    indent_level--;
    stream << indent() << "}\n";
    indent_level--;
    stream << indent() << ");\n";

    stream << indent() << "return {\n";
    indent_level++;
    for (auto const& out : out_info) {
        stream << indent() << "stub." << out.getter << ",\n";
    }
    stream << indent() << "stub.generator->get_target()\n";
    indent_level--;
    stream << indent() << "};\n";
    indent_level--;
    stream << indent() << "}\n";
    stream << "\n";

    stream << indent() << "// overload to allow GeneratorContext-pointer\n";
    stream << indent() << "inline static Outputs generate(\n";
    indent_level++;
    stream << indent() << "const GeneratorContext* context,\n";
    stream << indent() << "const Inputs& inputs,\n";
    stream << indent() << "const GeneratorParams& generator_params = GeneratorParams()\n";
    indent_level--;
    stream << indent() << ")\n";
    stream << indent() << "{\n";
    indent_level++;
    stream << indent() << "return generate(*context, inputs, generator_params);\n";
    indent_level--;
    stream << indent() << "}\n";
    stream << "\n";

    stream << indent() << "// overload to allow Target instead of GeneratorContext.\n";
    stream << indent() << "inline static Outputs generate(\n";
    indent_level++;
    stream << indent() << "const Target& target,\n";
    stream << indent() << "const Inputs& inputs,\n";
    stream << indent() << "const GeneratorParams& generator_params = GeneratorParams()\n";
    indent_level--;
    stream << indent() << ")\n";
    stream << indent() << "{\n";
    indent_level++;
    stream << indent() << "return generate(Halide::GeneratorContext(target), inputs, generator_params);\n";
    indent_level--;
    stream << indent() << "}\n";
    stream << "\n";

    stream << indent() << class_name << "() = delete;\n";

    indent_level--;
    stream << indent() << "};\n";
    stream << "\n";

    for (int i = static_cast<int>(namespaces.size()) - 1; i >= 0 ; --i) {
        stream << indent() << "}  // namespace " << namespaces[i] << "\n";
    }
    stream << "\n";

    stream << indent() << "#endif  // " << guard.str() << "\n";
}

GeneratorStub::GeneratorStub(const GeneratorContext &context,
                             GeneratorFactory generator_factory)
    : generator(generator_factory(context)) {}

GeneratorStub::GeneratorStub(const GeneratorContext &context,
                             GeneratorFactory generator_factory,
                             const GeneratorParamsMap &generator_params,
                             const std::vector<std::vector<Internal::StubInput>> &inputs)
    : GeneratorStub(context, generator_factory) {
    generate(generator_params, inputs);
}

// Return a vector of all Outputs of this Generator; non-array outputs are returned
// as a vector-of-size-1. This method is primarily useful for code that needs
// to iterate through the outputs of unknown, arbitrary Generators (e.g.,
// the Python bindings).
std::vector<std::vector<Func>> GeneratorStub::generate(const GeneratorParamsMap &generator_params,
                                                       const std::vector<std::vector<Internal::StubInput>> &inputs) {
    generator->set_generator_param_values(generator_params);
    generator->set_inputs_vector(inputs);
    Pipeline p = generator->build_pipeline();

    std::vector<std::vector<Func>> v;
    GeneratorBase::ParamInfo &pi = generator->param_info();
    if (!pi.filter_outputs.empty()) {
      for (auto output : pi.filter_outputs) {
          const std::string &name = output->name();
          if (output->is_array()) {
              v.push_back(get_array_output(name));
          } else {
              v.push_back(std::vector<Func>{get_output(name)});
          }
      }
    } else {
      // Generators with build() method can't have Output<>, hence can't have array outputs
      for (auto output : p.outputs()) {
          v.push_back(std::vector<Func>{output});
      }
    }
    return v;
}

GeneratorStub::Names GeneratorStub::get_names() const {
    auto &pi = generator->param_info();
    Names names;
    for (auto o : pi.generator_params) {
        names.generator_params.push_back(o->name);
    }
    for (auto o : pi.filter_params) {
        names.filter_params.push_back(o->name());
    }
    for (auto o : pi.filter_inputs) {
        names.inputs.push_back(o->name());
    }
    for (auto o : pi.filter_outputs) {
        names.outputs.push_back(o->name());
    }
    return names;
}

const std::map<std::string, Type> &get_halide_type_enum_map() {
    static const std::map<std::string, Type> halide_type_enum_map{
        {"bool", Bool()},
        {"int8", Int(8)},
        {"int16", Int(16)},
        {"int32", Int(32)},
        {"uint8", UInt(8)},
        {"uint16", UInt(16)},
        {"uint32", UInt(32)},
        {"float32", Float(32)},
        {"float64", Float(64)}
    };
    return halide_type_enum_map;
}

std::string halide_type_to_c_source(const Type &t) {
    static const std::map<halide_type_code_t, std::string> m = {
        { halide_type_int, "Int" },
        { halide_type_uint, "UInt" },
        { halide_type_float, "Float" },
        { halide_type_handle, "Handle" },
    };
    std::ostringstream oss;
    oss << "Halide::" << m.at(t.code()) << "(" << t.bits() << + ")";
    return oss.str();
}

std::string halide_type_to_c_type(const Type &t) {
    auto encode = [](const Type &t) -> int { return t.code() << 16 | t.bits(); };
    static const std::map<int, std::string> m = {
        { encode(Int(8)), "int8_t" },
        { encode(Int(16)), "int16_t" },
        { encode(Int(32)), "int32_t" },
        { encode(Int(64)), "int64_t" },
        { encode(UInt(1)), "bool" },
        { encode(UInt(8)), "uint8_t" },
        { encode(UInt(16)), "uint16_t" },
        { encode(UInt(32)), "uint32_t" },
        { encode(UInt(64)), "uint64_t" },
        { encode(Float(32)), "float" },
        { encode(Float(64)), "double" },
        { encode(Handle(64)), "void*" }
    };
    internal_assert(m.count(encode(t))) << t << " " << encode(t);
    return m.at(encode(t));
}

int generate_filter_main(int argc, char **argv, std::ostream &cerr) {
    const char kUsage[] = "gengen [-g GENERATOR_NAME] [-f FUNCTION_NAME] [-o OUTPUT_DIR] [-r RUNTIME_NAME] [-e EMIT_OPTIONS] [-x EXTENSION_OPTIONS] [-n FILE_BASE_NAME] "
                          "target=target-string[,target-string...] [generator_arg=value [...]]\n\n"
                          "  -e  A comma separated list of files to emit. Accepted values are "
                          "[assembly, bitcode, cpp, h, html, o, static_library, stmt, cpp_stub, schedule, yaml]. If omitted, default value is [static_library, h].\n"
                          "  -x  A comma separated list of file extension pairs to substitute during file naming, "
                          "in the form [.old=.new[,.old2=.new2]]\n";

    std::map<std::string, std::string> flags_info = { { "-f", "" },
                                                      { "-g", "" },
                                                      { "-o", "" },
                                                      { "-e", "" },
                                                      { "-n", "" },
                                                      { "-x", "" },
                                                      { "-r", "" }};
    GeneratorParamsMap generator_args;

    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] != '-') {
            std::vector<std::string> v = split_string(argv[i], "=");
            if (v.size() != 2 || v[0].empty() || v[1].empty()) {
                cerr << kUsage;
                return 1;
            }
            generator_args[v[0]] = v[1];
            continue;
        }
        auto it = flags_info.find(argv[i]);
        if (it != flags_info.end()) {
            if (i + 1 >= argc) {
                cerr << kUsage;
                return 1;
            }
            it->second = argv[i + 1];
            ++i;
            continue;
        }
        cerr << "Unknown flag: " << argv[i] << "\n";
        cerr << kUsage;
        return 1;
    }

    std::string runtime_name = flags_info["-r"];

    std::vector<std::string> generator_names = GeneratorRegistry::enumerate();
    if (generator_names.empty() && runtime_name.empty()) {
        cerr << "No generators have been registered and not compiling a standalone runtime\n";
        cerr << kUsage;
        return 1;
    }

    std::string generator_name = flags_info["-g"];
    if (generator_name.empty() && runtime_name.empty()) {
        // Require either -g or -r to be specified:
        // no longer infer the name when only one Generator is registered
        cerr << "Either -g <name> or -r must be specified; available Generators are:\n";
        if (!generator_names.empty()) {
            for (auto name : generator_names) {
                cerr << "    " << name << "\n";
            }
        } else {
            cerr << "    <none>\n";
        }
        return 1;
    }
    std::string function_name = flags_info["-f"];
    if (function_name.empty()) {
        // If -f isn't specified, assume function name = generator name.
        function_name = generator_name;
    }
    std::string output_dir = flags_info["-o"];
    if (output_dir.empty()) {
        cerr << "-o must always be specified.\n";
        cerr << kUsage;
        return 1;
    }

    /// It's ok to omit "target=" if we are generating a cpp_stub and/or yaml,
    /// but nothing else (e.g. no other emit options):
    const std::vector<std::string> emit_flags = split_string(flags_info["-e"], ",");
    
    /// I am not particularly proud of the next few lines:
    const bool stub_only = (emit_flags.size() == 1 &&   emit_flags[0] == "cpp_stub");
    const bool yaml_only = (emit_flags.size() == 1 &&   emit_flags[0] == "yaml");
    const bool stub_yaml = (emit_flags.size() == 2 && ((emit_flags[0] == "cpp_stub" && emit_flags[1] == "yaml") ||
                                                       (emit_flags[1] == "cpp_stub" && emit_flags[0] == "yaml")));
    const bool basic_emitters_only = stub_only ||
                                     yaml_only ||
                                     stub_yaml;
    
    if (!basic_emitters_only) {
        if (generator_args.find("target") == generator_args.end()) {
            cerr << "Target missing\n";
            cerr << kUsage;
            return 1;
        }
    }

    // it's OK for file_base_name to be empty: filename will be based on function name
    std::string file_base_name = flags_info["-n"];

    GeneratorBase::EmitOptions emit_options;
    // Ensure all flags start as false.
    emit_options.emit_static_library = emit_options.emit_h = false;

    if (emit_flags.empty() || (emit_flags.size() == 1 && emit_flags[0].empty())) {
        // If omitted or empty, assume .a and .h
        emit_options.emit_static_library = emit_options.emit_h = true;
    } else {
        // If anything specified, only emit what is enumerated
        for (const std::string &opt : emit_flags) {
            if (opt == "assembly") {
                emit_options.emit_assembly = true;
            } else if (opt == "bitcode") {
                emit_options.emit_bitcode = true;
            } else if (opt == "stmt") {
                emit_options.emit_stmt = true;
            } else if (opt == "html") {
                emit_options.emit_stmt_html = true;
            } else if (opt == "cpp") {
                emit_options.emit_cpp = true;
            } else if (opt == "py.c") {
                emit_options.emit_python_extension = true;
            } else if (opt == "o") {
                emit_options.emit_o = true;
            } else if (opt == "h") {
                emit_options.emit_h = true;
            } else if (opt == "static_library") {
                emit_options.emit_static_library = true;
            } else if (opt == "cpp_stub") {
                emit_options.emit_cpp_stub = true;
            } else if (opt == "schedule") {
                emit_options.emit_schedule = true;
            } else if (opt == "yaml") {
                emit_options.emit_yaml = true;
            } else if (!opt.empty()) {
                cerr << "Unrecognized emit option: " << opt
                     << " not one of [assembly, bitcode, cpp, h, html, o, static_library, stmt, cpp_stub, yaml], ignoring.\n";
            }
        }
    }

    auto substitution_flags = split_string(flags_info["-x"], ",");
    for (const std::string &x : substitution_flags) {
        if (x.empty()) {
            continue;
        }
        auto subst_pair = split_string(x, "=");
        if (subst_pair.size() != 2) {
            cerr << "Malformed -x option: " << x << "\n";
            cerr << kUsage;
            return 1;
        }
        emit_options.substitutions[subst_pair[0]] = subst_pair[1];
    }

    auto target_strings = split_string(generator_args["target"].string_value, ",");
    std::vector<Target> targets;
    for (const auto &s : target_strings) {
        targets.push_back(Target(s));
    }

    if (!runtime_name.empty()) {
        if (targets.size() != 1) {
            cerr << "Only one target allowed here";
            return 1;
        }
        std::string base_path = compute_base_path(output_dir, runtime_name, "");
        Outputs output_files = compute_outputs(targets[0], base_path, emit_options);
        compile_standalone_runtime(output_files, targets[0]);
    }

    if (!generator_name.empty()) {
        std::string base_path = compute_base_path(output_dir, function_name, file_base_name);
        debug(1) << "Generator " << generator_name << " has base_path " << base_path << "\n";
        
        if (emit_options.emit_cpp_stub || emit_options.emit_yaml) {
            /// When generating cpp_stubs and/or YAML metadata, we ignore all generator args passed in,
            /// and supply a fake placeholder Target argument:
            auto gen = GeneratorRegistry::create(generator_name, GeneratorContext(Target()));
            if (emit_options.emit_cpp_stub) {
                std::string stub_file_path = base_path + get_extension(".stub.h", emit_options);
                gen->emit_cpp_stub(stub_file_path);
            }
            if (emit_options.emit_yaml) {
                std::string yaml_file_path = base_path + get_extension(".yaml", emit_options);
                gen->emit_yaml(yaml_file_path);
            }
        }
        
        /// Don't bother with this if we're just emitting via “basic emitter” (e.g. YAML or a cpp_stub).
        if (!basic_emitters_only) {
            Outputs output_files = compute_outputs(targets[0], base_path, emit_options);
            auto module_producer = [&generator_name, &generator_args]
                (const std::string &name, const Target &target) -> Module {
                    auto sub_generator_args = generator_args;
                    sub_generator_args.erase("target");
                    // Must re-create each time since each instance will have a different Target.
                    auto gen = GeneratorRegistry::create(generator_name, GeneratorContext(target));
                    gen->set_generator_param_values(sub_generator_args);
                    return gen->build_module(name);
                };
            if (targets.size() > 1 || !emit_options.substitutions.empty()) {
                compile_multitarget(function_name, output_files, targets, module_producer, emit_options.substitutions);
            } else {
                user_assert(emit_options.substitutions.empty()) << "substitutions not supported for single-target";
                // compile_multitarget() will fail if we request anything but library and/or header,
                // so defer directly to Module::compile if there is a single target.
                module_producer(function_name, targets[0]).compile(output_files);
            }
        }
    }

    return 0;
}

GeneratorParamBase::GeneratorParamBase(const std::string &name) : name(name) {
    ObjectInstanceRegistry::register_instance(this, 0, ObjectInstanceRegistry::GeneratorParam,
                                              this, nullptr);
}

GeneratorParamBase::~GeneratorParamBase() { ObjectInstanceRegistry::unregister_instance(this); }

void GeneratorParamBase::check_value_readable() const {
    // These are always readable.
    if (name == "target") return;
    if (name == "auto_schedule") return;
    if (name == "machine_params") return;
    user_assert(generator && generator->phase >= GeneratorBase::GenerateCalled)  << "The GeneratorParam \"" << name << "\" cannot be read before build() or generate() is called.\n";
}

void GeneratorParamBase::check_value_writable() const {
    // Allow writing when no Generator is set, to avoid having to special-case ctor initing code
    if (!generator) return;
    user_assert(generator->phase < GeneratorBase::GenerateCalled)  << "The GeneratorParam \"" << name << "\" cannot be written after build() or generate() is called.\n";
}

void GeneratorParamBase::fail_wrong_type(const char *type) {
    user_error << "The GeneratorParam \"" << name << "\" cannot be set with a value of type " << type << ".\n";
}

/* static */
GeneratorRegistry &GeneratorRegistry::get_registry() {
    static GeneratorRegistry *registry = new GeneratorRegistry;
    return *registry;
}

/* static */
void GeneratorRegistry::register_factory(const std::string &name,
                                         GeneratorFactory generator_factory) {
    user_assert(is_valid_name(name)) << "Invalid Generator name: " << name;
    GeneratorRegistry &registry = get_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    internal_assert(registry.factories.find(name) == registry.factories.end())
        << "Duplicate Generator name: " << name;
    registry.factories[name] = generator_factory;
}

/* static */
void GeneratorRegistry::unregister_factory(const std::string &name) {
    GeneratorRegistry &registry = get_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    internal_assert(registry.factories.find(name) != registry.factories.end())
        << "Generator not found: " << name;
    registry.factories.erase(name);
}

/* static */
std::unique_ptr<GeneratorBase> GeneratorRegistry::create(const std::string &name,
                                                         const GeneratorContext &context) {
    GeneratorRegistry &registry = get_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    auto it = registry.factories.find(name);
    if (it == registry.factories.end()) {
        std::ostringstream o;
        o << "Generator not found: " << name << "\n";
        o << "Did you mean:\n";
        for (const auto &n : registry.factories) {
            o << "    " << n.first << "\n";
        }
        user_error << o.str();
    }
    std::unique_ptr<GeneratorBase> g = it->second(context);
    internal_assert(g != nullptr);
    return g;
}

/* static */
std::vector<std::string> GeneratorRegistry::enumerate() {
    GeneratorRegistry &registry = get_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    std::vector<std::string> result;
    for (const auto& i : registry.factories) {
        result.push_back(i.first);
    }
    return result;
}

GeneratorBase::GeneratorBase(size_t size, const void *introspection_helper)
    : size(size) {
    ObjectInstanceRegistry::register_instance(this, size, ObjectInstanceRegistry::Generator, this, introspection_helper);
}

GeneratorBase::~GeneratorBase() {
    ObjectInstanceRegistry::unregister_instance(this);
}

GeneratorBase::ParamInfo::ParamInfo(GeneratorBase *generator, const size_t size) {
    std::set<std::string> names;
    std::vector<void *> vf = ObjectInstanceRegistry::instances_in_range(
        generator, size, ObjectInstanceRegistry::FilterParam);
    for (auto v : vf) {
        auto rp = static_cast<RegisteredParameter *>(v);
        internal_assert(rp != nullptr && rp->defined());
        user_assert(rp->is_explicit_name()) << "Params in Generators must have explicit names: " << rp->name();
        user_assert(is_valid_name(rp->name())) << "Invalid Param name: " << rp->name();
        user_assert(!names.count(rp->name())) << "Duplicate Param name: " << rp->name();
        names.insert(rp->name());
        filter_params.push_back(rp);
    }

    const auto add_synthetic_params = [this, generator](GIOBase *gio) {
        const std::string &n = gio->name();
        const std::string &gn = generator->generator_registered_name;

        if (gio->kind() != IOKind::Scalar) {
            owned_synthetic_params.push_back(GeneratorParam_Synthetic<Type>::make(generator, gn, n + ".type", *gio, SyntheticParamType::Type, gio->types_defined()));
            generator_params.push_back(owned_synthetic_params.back().get());

            owned_synthetic_params.push_back(GeneratorParam_Synthetic<int>::make(generator, gn, n + ".dim", *gio, SyntheticParamType::Dim, gio->dims_defined()));
            generator_params.push_back(owned_synthetic_params.back().get());
        }
        if (gio->is_array()) {
            owned_synthetic_params.push_back(GeneratorParam_Synthetic<size_t>::make(generator, gn, n + ".size", *gio, SyntheticParamType::ArraySize, gio->array_size_defined()));
            generator_params.push_back(owned_synthetic_params.back().get());
        }
    };

    std::vector<void *> vi = ObjectInstanceRegistry::instances_in_range(
        generator, size, ObjectInstanceRegistry::GeneratorInput);
    for (auto v : vi) {
        auto input = static_cast<Internal::GeneratorInputBase *>(v);
        internal_assert(input != nullptr);
        user_assert(is_valid_name(input->name())) << "Invalid Input name: (" << input->name() << ")\n";
        user_assert(!names.count(input->name())) << "Duplicate Input name: " << input->name();
        names.insert(input->name());
        internal_assert(input->generator == nullptr || input->generator == generator);
        input->generator = generator;
        filter_inputs.push_back(input);
        add_synthetic_params(input);
    }

    std::vector<void *> vo = ObjectInstanceRegistry::instances_in_range(
        generator, size, ObjectInstanceRegistry::GeneratorOutput);
    for (auto v : vo) {
        auto output = static_cast<Internal::GeneratorOutputBase *>(v);
        internal_assert(output != nullptr);
        user_assert(is_valid_name(output->name())) << "Invalid Output name: (" << output->name() << ")\n";
        user_assert(!names.count(output->name())) << "Duplicate Output name: " << output->name();
        names.insert(output->name());
        internal_assert(output->generator == nullptr || output->generator == generator);
        output->generator = generator;
        filter_outputs.push_back(output);
        add_synthetic_params(output);
    }

    if (filter_params.size() > 0 && filter_inputs.size() > 0) {
        user_error << "Input<> may not be used with Param<> or ImageParam in Generators.\n";
    }

    if (filter_params.size() > 0 && filter_outputs.size() > 0) {
        user_error << "Output<> may not be used with Param<> or ImageParam in Generators.\n";
    }

    std::vector<void *> vg = ObjectInstanceRegistry::instances_in_range(
        generator, size, ObjectInstanceRegistry::GeneratorParam);
    for (auto v : vg) {
        auto param = static_cast<GeneratorParamBase *>(v);
        internal_assert(param != nullptr);
        user_assert(is_valid_name(param->name)) << "Invalid GeneratorParam name: " << param->name;
        user_assert(!names.count(param->name)) << "Duplicate GeneratorParam name: " << param->name;
        names.insert(param->name);
        internal_assert(param->generator == nullptr || param->generator == generator);
        param->generator = generator;
        generator_params.push_back(param);
    }

    // Do in separate loop so that synthetic params are also included
    for (auto *g : generator_params) {
        generator_params_by_name[g->name] = g;
    }

    for (auto &g : owned_synthetic_params) {
        g->generator = generator;
    }
}

GeneratorBase::ParamInfo &GeneratorBase::param_info() {
    if (!param_info_ptr) {
        param_info_ptr.reset(new ParamInfo(this, size));
    }
    return *param_info_ptr;
}

Func GeneratorBase::get_output(const std::string &n) {
    check_min_phase(GenerateCalled);
    auto *output = find_output_by_name(n);
    // Call for the side-effect of asserting if the value isn't defined.
    (void) output->array_size();
    user_assert(!output->is_array() && output->funcs().size() == 1) << "Output " << n << " must be accessed via get_array_output()\n";
    Func f = output->funcs().at(0);
    user_assert(f.defined()) << "Output " << n << " was not defined.\n";
    return f;
}

std::vector<Func> GeneratorBase::get_array_output(const std::string &n) {
    check_min_phase(GenerateCalled);
    auto *output = find_output_by_name(n);
    // Call for the side-effect of asserting if the value isn't defined.
    (void) output->array_size();
    for (const auto &f : output->funcs()) {
        user_assert(f.defined()) << "Output " << n << " was not fully defined.\n";
    }
    return output->funcs();
}

// Find output by name. If not found, assert-fail. Never returns null.
GeneratorOutputBase *GeneratorBase::find_output_by_name(const std::string &name) {
    // There usually are very few outputs, so a linear search is fine
    ParamInfo &pi = param_info();
    for (GeneratorOutputBase *output : pi.filter_outputs) {
        if (output->name() == name) {
            return output;
        }
    }
    internal_error << "Output " << name << " not found.";
    return nullptr;  // not reached
}

void GeneratorBase::set_generator_param_values(const GeneratorParamsMap &params) {
    ParamInfo &pi = param_info();
    for (auto &key_value : params) {
        auto gp = pi.generator_params_by_name.find(key_value.first);
        if (gp != pi.generator_params_by_name.end()) {
            if (gp->second->is_looplevel_param()) {
                if (!key_value.second.string_value.empty()) {
                    gp->second->set_from_string(key_value.second.string_value);
                } else {
                    gp->second->set(key_value.second.loop_level);
                }
            } else {
                gp->second->set_from_string(key_value.second.string_value);
            }
            continue;
        }
        user_error << "Generator " << generator_registered_name << " has no GeneratorParam named: " << key_value.first << "\n";
    }
}

void GeneratorBase::init_from_context(const Halide::GeneratorContext &context) {
  Halide::GeneratorContext::init_from_context(context);
  // pre-emptively build our param_info now
  (void) this->param_info();
}

void GeneratorBase::set_generator_names(const std::string &registered_name, const std::string &stub_name) {
    user_assert(is_valid_name(registered_name)) << "Invalid Generator name: " << registered_name;
    internal_assert(!registered_name.empty() && !stub_name.empty());
    internal_assert(generator_registered_name.empty() && generator_stub_name.empty());
    generator_registered_name = registered_name;
    generator_stub_name = stub_name;
}

void GeneratorBase::set_inputs_vector(const std::vector<std::vector<StubInput>> &inputs) {
    advance_phase(InputsSet);
    internal_assert(!inputs_set) << "set_inputs_vector() must be called at most once per Generator instance.\n";
    ParamInfo &pi = param_info();
    user_assert(pi.filter_params.empty())
        << "The set_inputs_vector() method cannot be used for Generators that use Param<> or ImageParam.";
    user_assert(inputs.size() == pi.filter_inputs.size())
            << "Expected exactly " << pi.filter_inputs.size()
            << " inputs but got " << inputs.size() << "\n";
    for (size_t i = 0; i < pi.filter_inputs.size(); ++i) {
        pi.filter_inputs[i]->set_inputs(inputs[i]);
    }
    inputs_set = true;
}

void GeneratorBase::track_parameter_values(bool include_outputs) {
    ParamInfo &pi = param_info();
    for (auto input : pi.filter_inputs) {
        if (input->kind() == IOKind::Buffer) {
            internal_assert(!input->parameters_.empty());
            for (auto &p : input->parameters_) {
                // This must use p.name(), *not* input->name()
                get_value_tracker()->track_values(p.name(), parameter_constraints(p));
            }
        }
    }
    if (include_outputs) {
        for (auto output : pi.filter_outputs) {
            if (output->kind() == IOKind::Buffer) {
                internal_assert(!output->funcs().empty());
                for (auto &f : output->funcs()) {
                    user_assert(f.defined()) << "Output " << output->name() << " is not fully defined.";
                    Parameter p = f.output_buffer().parameter();
                    // This must use p.name(), *not* output->name()
                    get_value_tracker()->track_values(p.name(), parameter_constraints(p));
                }
            }
        }
    }
}

void GeneratorBase::check_min_phase(Phase expected_phase) const {
    user_assert(phase >= expected_phase) << "You may not do this operation at this phase.";
}

void GeneratorBase::check_exact_phase(Phase expected_phase) const {
    user_assert(phase == expected_phase) << "You may not do this operation at this phase.";
}

void GeneratorBase::advance_phase(Phase new_phase) {
    switch (new_phase) {
    case Created:
        internal_error << "Impossible";
        break;
    case InputsSet:
        internal_assert(phase == Created);
        break;
    case GenerateCalled:
        // It's OK to advance from Created to GenerateCalled, skipping InputsSet.
        internal_assert(phase == Created || phase == InputsSet);
        break;
    case ScheduleCalled:
        internal_assert(phase == GenerateCalled);
        break;
    }
    phase = new_phase;
}


void GeneratorBase::pre_generate() {
    advance_phase(GenerateCalled);
    ParamInfo &pi = param_info();
    user_assert(pi.filter_params.empty()) << "May not use generate() method with Param<> or ImageParam.";
    user_assert(pi.filter_outputs.size() > 0) << "Must use Output<> with generate() method.";
    user_assert(get_target() != Target()) << "The Generator target has not been set.";

    if (!inputs_set) {
        for (auto input : pi.filter_inputs) {
            input->init_internals();
        }
        inputs_set = true;
    }
    for (auto output : pi.filter_outputs) {
        output->init_internals();
    }
    track_parameter_values(false);
}

void GeneratorBase::post_generate() {
    track_parameter_values(true);
}

void GeneratorBase::pre_schedule() {
    advance_phase(ScheduleCalled);
    track_parameter_values(true);
}

void GeneratorBase::post_schedule() {
    track_parameter_values(true);
}

void GeneratorBase::pre_build() {
    advance_phase(GenerateCalled);
    advance_phase(ScheduleCalled);
    ParamInfo &pi = param_info();
    user_assert(pi.filter_outputs.empty()) << "May not use build() method with Output<>.";
    if (!inputs_set) {
        for (auto input : pi.filter_inputs) {
            input->init_internals();
        }
        inputs_set = true;
    }
    track_parameter_values(false);
}

void GeneratorBase::post_build() {
    track_parameter_values(true);
}

Pipeline GeneratorBase::get_pipeline() {
    check_min_phase(GenerateCalled);
    if (!pipeline.defined()) {
        ParamInfo &pi = param_info();
        user_assert(pi.filter_outputs.size() > 0) << "Must use get_pipeline<> with Output<>.";
        std::vector<Func> funcs;
        for (auto output : pi.filter_outputs) {
            for (const auto &f : output->funcs()) {
                user_assert(f.defined()) << "Output \"" << f.name() << "\" was not defined.\n";
                if (output->dims_defined()) {
                    user_assert(f.dimensions() == output->dims()) << "Output \"" << f.name()
                        << "\" requires dimensions=" << output->dims()
                        << " but was defined as dimensions=" << f.dimensions() << ".\n";
                }
                if (output->types_defined()) {
                    user_assert((int)f.outputs() == (int)output->types().size()) << "Output \"" << f.name()
                            << "\" requires a Tuple of size " << output->types().size()
                            << " but was defined as Tuple of size " << f.outputs() << ".\n";
                    for (size_t i = 0; i < f.output_types().size(); ++i) {
                        Type expected = output->types().at(i);
                        Type actual = f.output_types()[i];
                        user_assert(expected == actual) << "Output \"" << f.name()
                            << "\" requires type " << expected
                            << " but was defined as type " << actual << ".\n";
                    }
                }
                funcs.push_back(f);
            }
        }
        pipeline = Pipeline(funcs);
    }
    return pipeline;
}

Module GeneratorBase::build_module(const std::string &function_name,
                                   const LinkageType linkage_type) {
    std::string auto_schedule_result;
    Pipeline pipeline = build_pipeline();
    if (get_auto_schedule()) {
        auto_schedule_result = pipeline.auto_schedule(get_target(), get_machine_params());
    }

    // Special-case here: for certain legacy Generators, building the pipeline
    // can mutate the Params/ImageParams (mainly, to customize the type/dim
    // of an ImageParam based on a GeneratorParam); to handle these, we discard (and rebuild)
    // the ParamInfo for all "old-style" Generators. This isn't really desirable
    // and hopefully can be eliminated someday.
    if (param_info().filter_params.size() > 0) {
        param_info_ptr.reset();
    }

    ParamInfo &pi = param_info();
    std::vector<Argument> filter_arguments;
    for (auto rp : pi.filter_params) {
        filter_arguments.push_back(to_argument(*rp));
    }
    for (auto input : pi.filter_inputs) {
        for (const auto &p : input->parameters_) {
            filter_arguments.push_back(to_argument(p));
        }
    }

    Module result = pipeline.compile_to_module(filter_arguments, function_name, target, linkage_type);
    std::shared_ptr<ExternsMap> externs_map = get_externs_map();
    for (const auto &map_entry : *externs_map) {
        result.append(map_entry.second);
    }

    auto outputs = pipeline.outputs();
    for (auto *output : pi.filter_outputs) {
        for (size_t i = 0; i < output->funcs().size(); ++i) {
            auto from = output->funcs()[i].name();
            auto to = output->array_name(i);
            size_t tuple_size = output->types_defined() ? output->types().size() : 1;
            for (size_t t = 0; t < tuple_size; ++t) {
                std::string suffix = (tuple_size > 1) ? ("." + std::to_string(t)) : "";
                result.remap_metadata_name(from + suffix, to + suffix);
            }
        }
    }

    result.set_auto_schedule(auto_schedule_result);

    return result;
}

void GeneratorBase::emit_cpp_stub(std::string const& stub_file_path) {
    user_assert(!generator_registered_name.empty() && !generator_stub_name.empty()) << "Generator has no name.\n";
    /// StubEmitter will want to access the GP/SP values, so advance the phase to avoid assert-fails.
    advance_phase(GenerateCalled);
    advance_phase(ScheduleCalled);
    ParamInfo& pi = param_info();
    std::ofstream file(stub_file_path);
    /// Construct a StubEmitter for this GeneratorBase instance:
    StubEmitter stubmitter(file,
                           generator_registered_name,
                           generator_stub_name,
                           pi.generator_params,
                           pi.filter_inputs,
                           pi.filter_outputs);
    /// Emit the C++ stub:
    stubmitter.emit();
}

void GeneratorBase::emit_yaml(std::string const& yaml_file_path) {
    user_assert(!generator_registered_name.empty() && !generator_stub_name.empty()) << "Generator has no name.\n";
    /// YamlEmitter will want to access the GP/SP values, so advance the phase to avoid assert-fails.
    advance_phase(GenerateCalled);
    advance_phase(ScheduleCalled);
    ParamInfo& pi = param_info();
    std::ofstream file(yaml_file_path);
    /// Construct a YamlEmitter for this GeneratorBase instance:
    YamlEmitter yamitter(file,
                         generator_registered_name,
                         generator_stub_name,
                         pi.generator_params,
                         pi.filter_inputs,
                         pi.filter_outputs);
    /// Emit the YAML metadata:
    yamitter.emit();
}

void GeneratorBase::check_scheduled(const char* m) const {
    check_min_phase(ScheduleCalled);
}

void GeneratorBase::check_input_is_singular(Internal::GeneratorInputBase *in) {
    user_assert(!in->is_array())
        << "Input " << in->name() << " is an array, and must be set with a vector type.";
}

void GeneratorBase::check_input_is_array(Internal::GeneratorInputBase *in) {
    user_assert(in->is_array())
        << "Input " << in->name() << " is not an array, and must not be set with a vector type.";
}

void GeneratorBase::check_input_kind(Internal::GeneratorInputBase *in, Internal::IOKind kind) {
    user_assert(in->kind() == kind)
        << "Input " << in->name() << " cannot be set with the type specified.";
}

GIOBase::GIOBase(size_t array_size,
                 const std::string &name,
                 IOKind kind,
                 const std::vector<Type> &types,
                 int dims)
    : array_size_(array_size), name_(name), kind_(kind), types_(types), dims_(dims) {
}

GIOBase::~GIOBase() {
    // nothing
}

bool GIOBase::array_size_defined() const {
    return array_size_ != -1;
}

size_t GIOBase::array_size() const {
    user_assert(array_size_defined()) << "ArraySize is unspecified for " << input_or_output() <<
        "'" << name() << "'; you need to explicitly set it via the resize() method or by setting '"
        << name() << ".size' in your build rules.";
    return (size_t) array_size_;
}

bool GIOBase::is_array() const {
    internal_error << "Unimplemented"; return false;
}

const std::string &GIOBase::name() const {
    return name_;
}

IOKind GIOBase::kind() const {
    return kind_;
}

bool GIOBase::types_defined() const {
    return !types_.empty();
}

const std::vector<Type> &GIOBase::types() const {
    // If types aren't defined, but we have one Func that is,
    // we probably just set an Output<Func> and should propagate the types.
    if (!types_defined()) {
        // use funcs_, not funcs(): the latter could give a much-less-helpful error message
        // in this case.
        const auto &f = funcs_;
        if (f.size() == 1 && f.at(0).defined()) {
            check_matching_types(f.at(0).output_types());
        }
    }
    user_assert(types_defined()) << "Type is not defined for " << input_or_output() <<
        " '" << name() << "'; you may need to specify '" << name() << ".type' as a GeneratorParam.\n";
    return types_;
}

Type GIOBase::type() const {
    const auto &t = types();
    internal_assert(t.size() == 1) << "Expected types_.size() == 1, saw " << t.size() << " for " << name() << "\n";
    return t.at(0);
}

bool GIOBase::dims_defined() const {
    return dims_ != -1;
}

int GIOBase::dims() const {
    // If types aren't defined, but we have one Func that is,
    // we probably just set an Output<Func> and should propagate the types.
    if (!dims_defined()) {
        // use funcs_, not funcs(): the latter could give a much-less-helpful error message
        // in this case.
        const auto &f = funcs_;
        if (f.size() == 1 && f.at(0).defined()) {
            check_matching_dims(funcs().at(0).dimensions());
        }
    }
    user_assert(dims_defined()) << "Dimensions are not defined for " << input_or_output() <<
        " '" << name() << "'; you may need to specify '" << name() << ".dim' as a GeneratorParam.\n";
    return dims_;
}

const std::vector<Func> &GIOBase::funcs() const {
    internal_assert(funcs_.size() == array_size() && exprs_.empty());
    return funcs_;
}

const std::vector<Expr> &GIOBase::exprs() const {
    internal_assert(exprs_.size() == array_size() && funcs_.empty());
    return exprs_;
}

void GIOBase::verify_internals() {
    user_assert(dims_ >= 0) << "Generator Input/Output Dimensions must have positive values";

    if (kind() != IOKind::Scalar) {
        for (const Func &f : funcs()) {
            user_assert(f.defined()) << "Input/Output " << name() << " is not defined.\n";
            user_assert(f.dimensions() == dims())
                << "Expected dimensions " << dims()
                << " but got " << f.dimensions()
                << " for " << name() << "\n";
            user_assert(f.outputs() == 1)
                << "Expected outputs() == " << 1
                << " but got " << f.outputs()
                << " for " << name() << "\n";
            user_assert(f.output_types().size() == 1)
                << "Expected output_types().size() == " << 1
                << " but got " << f.outputs()
                << " for " << name() << "\n";
            user_assert(f.output_types()[0] == type())
                << "Expected type " << type()
                << " but got " << f.output_types()[0]
                << " for " << name() << "\n";
        }
    } else {
        for (const Expr &e : exprs()) {
            user_assert(e.defined()) << "Input/Ouput " << name() << " is not defined.\n";
            user_assert(e.type() == type())
                << "Expected type " << type()
                << " but got " << e.type()
                << " for " << name() << "\n";
        }
    }
}

std::string GIOBase::array_name(size_t i) const {
    std::string n = name();
    if (is_array()) {
        n += "_" + std::to_string(i);
    }
    return n;
}

// If our type(s) are defined, ensure it matches the ones passed in, asserting if not.
// If our type(s) are not defined, just set to the ones passed in.
void GIOBase::check_matching_types(const std::vector<Type> &t) const {
    if (types_defined()) {
        user_assert(types().size() == t.size()) << "Type mismatch for " << name() << ": expected " << types().size() << " types but saw " << t.size();
        for (size_t i = 0; i < t.size(); ++i) {
            user_assert(types().at(i) == t.at(i)) << "Type mismatch for " << name() << ": expected " << types().at(i) << " saw " << t.at(i);
        }
    } else {
        types_ = t;
    }
}

// If our dims are defined, ensure it matches the one passed in, asserting if not.
// If our dims are not defined, just set to the one passed in.
void GIOBase::check_matching_dims(int d) const {
    internal_assert(d >= 0);
    if (dims_defined()) {
        user_assert(dims() == d) << "Dimensions mismatch for " << name() << ": expected " << dims() << " saw " << d;
    } else {
        dims_ = d;
    }
}

void GIOBase::check_matching_array_size(size_t size) const {
    if (array_size_defined()) {
        user_assert(array_size() == size) << "ArraySize mismatch for " << name() << ": expected " << array_size() << " saw " << size;
    } else {
        array_size_ = size;
    }
}

GeneratorInputBase::GeneratorInputBase(size_t array_size,
                                       const std::string &name,
                                       IOKind kind,
                                       const std::vector<Type> &t,
                                       int d)
    : GIOBase(array_size, name, kind, t, d) {
    ObjectInstanceRegistry::register_instance(this, 0, ObjectInstanceRegistry::GeneratorInput, this, nullptr);
}

GeneratorInputBase::GeneratorInputBase(const std::string &name, IOKind kind, const std::vector<Type> &t, int d)
    : GeneratorInputBase(1, name, kind, t, d) {
    // nothing
}

GeneratorInputBase::~GeneratorInputBase() {
    ObjectInstanceRegistry::unregister_instance(this);
}

void GeneratorInputBase::check_value_writable() const {
    user_assert(generator && generator->phase == GeneratorBase::InputsSet)  << "The Input " << name() << " cannot be set at this point.\n";
}

void GeneratorInputBase::set_def_min_max() {
    // nothing
}

Parameter GeneratorInputBase::parameter() const {
    user_assert(!this->is_array()) << "Cannot call the parameter() method on Input<[]> " << name() << "; use an explicit subscript operator instead.";
    return parameters_.at(0);
}

void GeneratorInputBase::verify_internals() {
    GIOBase::verify_internals();

    const size_t expected = (kind() != IOKind::Scalar) ? funcs().size() : exprs().size();
    user_assert(parameters_.size() == expected) << "Expected parameters_.size() == "
        << expected << ", saw " << parameters_.size() << " for " << name() << "\n";
}

void GeneratorInputBase::init_internals() {
    // Call these for the side-effect of asserting if the values aren't defined.
    (void) array_size();
    (void) types();
    (void) dims();

    parameters_.clear();
    exprs_.clear();
    funcs_.clear();
    for (size_t i = 0; i < array_size(); ++i) {
        auto name = array_name(i);
        parameters_.emplace_back(type(), kind() != IOKind::Scalar, dims(), name, true);
        auto &p = parameters_[i];
        if (kind() != IOKind::Scalar) {
            internal_assert(dims() == p.dimensions());
            funcs_.push_back(make_param_func(p, name));
        } else {
            Expr e = Internal::Variable::make(type(), name, p);
            exprs_.push_back(e);
        }
    }

    set_def_min_max();
    verify_internals();
}

void GeneratorInputBase::set_inputs(const std::vector<StubInput> &inputs) {
    generator->check_exact_phase(GeneratorBase::InputsSet);
    parameters_.clear();
    exprs_.clear();
    funcs_.clear();
    check_matching_array_size(inputs.size());
    for (size_t i = 0; i < inputs.size(); ++i) {
        const StubInput &in = inputs.at(i);
        user_assert(in.kind() == kind()) << "An input for " << name() << " is not of the expected kind.\n";
        if (kind() == IOKind::Function) {
            auto f = in.func();
            check_matching_types(f.output_types());
            check_matching_dims(f.dimensions());
            funcs_.push_back(f);
            parameters_.emplace_back(f.output_types().at(0), true, f.dimensions(), array_name(i), true);
        } else if (kind() == IOKind::Buffer) {
            auto p = in.parameter();
            check_matching_types({p.type()});
            check_matching_dims(p.dimensions());
            funcs_.push_back(make_param_func(p, name()));
            parameters_.push_back(p);
        } else {
            auto e = in.expr();
            check_matching_types({e.type()});
            check_matching_dims(0);
            exprs_.push_back(e);
            parameters_.emplace_back(e.type(), false, 0, array_name(i), true);
        }
    }

    set_def_min_max();
    verify_internals();
}

void GeneratorInputBase::estimate_impl(Var var, Expr min, Expr extent) {
    internal_assert(exprs_.empty() && funcs_.size() > 0 && parameters_.size() == funcs_.size());
    for (size_t i = 0; i < funcs_.size(); ++i) {
        Func &f = funcs_[i];
        f.estimate(var, min, extent);
        // Propagate the estimate into the Parameter as well, just in case
        // we end up compiling this for toplevel.
        std::vector<Var> args = f.args();
        int dim = -1;
        for (size_t a = 0; a < args.size(); ++a) {
            if (args[a].same_as(var)) {
                dim = a;
                break;
            }
        }
        internal_assert(dim >= 0);
        Parameter &p = parameters_[i];
        p.set_min_constraint_estimate(dim, min);
        p.set_extent_constraint_estimate(dim, extent);
    }
}

GeneratorOutputBase::GeneratorOutputBase(size_t array_size, const std::string &name, IOKind kind, const std::vector<Type> &t, int d)
    : GIOBase(array_size, name, kind, t, d) {
    internal_assert(kind != IOKind::Scalar);
    ObjectInstanceRegistry::register_instance(this, 0, ObjectInstanceRegistry::GeneratorOutput,
                                              this, nullptr);
}

GeneratorOutputBase::GeneratorOutputBase(const std::string &name, IOKind kind, const std::vector<Type> &t, int d)
    : GeneratorOutputBase(1, name, kind, t, d) {
    // nothing
}

GeneratorOutputBase::~GeneratorOutputBase() {
    ObjectInstanceRegistry::unregister_instance(this);
}

void GeneratorOutputBase::check_value_writable() const {
    user_assert(generator && generator->phase == GeneratorBase::GenerateCalled)  << "The Output " << name() << " can only be set inside generate().\n";
}

void GeneratorOutputBase::init_internals() {
    exprs_.clear();
    funcs_.clear();
    if (array_size_defined()) {
        for (size_t i = 0; i < array_size(); ++i) {
            funcs_.push_back(Func(array_name(i)));
        }
    }
}

void GeneratorOutputBase::resize(size_t size) {
    internal_assert(is_array());
    internal_assert(!array_size_defined()) << "You may only call " << name()
        << ".resize() when then size is undefined\n";
    array_size_ = (int) size;
    init_internals();
}

void StubOutputBufferBase::check_scheduled(const char* m) const {
    generator->check_scheduled(m);
}

Target StubOutputBufferBase::get_target() const {
    return generator->get_target();
}

void generator_test() {
    GeneratorContext context(get_host_target());

    // Verify that the Generator's internal phase actually prevents unsupported
    // order of operations.
    {
        class Tester : public Generator<Tester> {
        public:
            GeneratorParam<int> gp0{"gp0", 0};
            GeneratorParam<float> gp1{"gp1", 1.f};
            GeneratorParam<uint64_t> gp2{"gp2", 2};

            Input<int> input{"input"};
            Output<Func> output{"output", Int(32), 1};

            void generate() {
                internal_assert(gp0 == 1);
                internal_assert(gp1 == 2.f);
                internal_assert(gp2 == (uint64_t) 2);  // unchanged
                Var x;
                output(x) = input + gp0;
            }
            void schedule() {
                // empty
            }
        };

        Tester tester;
        tester.init_from_context(context);
        internal_assert(tester.phase == GeneratorBase::Created);

        // Verify that calling GeneratorParam::set() works.
        tester.gp0.set(1);

        tester.set_inputs_vector({{StubInput(42)}});
        internal_assert(tester.phase == GeneratorBase::InputsSet);

        // tester.set_inputs_vector({{StubInput(43)}});  // This will assert-fail.

        // Also ok to call in this phase.
        tester.gp1.set(2.f);

        tester.call_generate();
        internal_assert(tester.phase == GeneratorBase::GenerateCalled);

        // tester.set_inputs_vector({{StubInput(44)}});  // This will assert-fail.
        // tester.gp2.set(2);  // This will assert-fail.

        tester.call_schedule();
        internal_assert(tester.phase == GeneratorBase::ScheduleCalled);

        // tester.set_inputs_vector({{StubInput(45)}});  // This will assert-fail.
        // tester.gp2.set(2);  // This will assert-fail.
        // tester.sp2.set(202);  // This will assert-fail.
    }

    // Verify that the Generator's internal phase actually prevents unsupported
    // order of operations (with old-style Generator)
    {
        class Tester : public Generator<Tester> {
        public:
            GeneratorParam<int> gp0{"gp0", 0};
            GeneratorParam<float> gp1{"gp1", 1.f};
            GeneratorParam<uint64_t> gp2{"gp2", 2};
            GeneratorParam<uint8_t> gp_uint8{"gp_uint8", 65};
            GeneratorParam<int8_t> gp_int8{"gp_int8", 66};
            GeneratorParam<char> gp_char{"gp_char", 97};
            GeneratorParam<signed char> gp_schar{"gp_schar", 98};
            GeneratorParam<unsigned char> gp_uchar{"gp_uchar", 99};
            GeneratorParam<bool> gp_bool{"gp_bool", true};

            Input<int> input{"input"};

            Func build() {
                internal_assert(gp0 == 1);
                internal_assert(gp1 == 2.f);
                internal_assert(gp2 == (uint64_t) 2);  // unchanged
                internal_assert(gp_uint8 == 67);
                internal_assert(gp_int8 == 68);
                internal_assert(gp_bool == false);
                internal_assert(gp_char == 107);
                internal_assert(gp_schar == 108);
                internal_assert(gp_uchar == 109);
                Var x;
                Func output;
                output(x) = input + gp0;
                return output;
            }
        };

        Tester tester;
        tester.init_from_context(context);
        internal_assert(tester.phase == GeneratorBase::Created);

        // Verify that calling GeneratorParam::set() works.
        tester.gp0.set(1);

        // set_inputs_vector() can't be called on an old-style Generator;
        // that's OK, since we can skip from Created -> GenerateCalled anyway
        // tester.set_inputs_vector({{StubInput(42)}});
        // internal_assert(tester.phase == GeneratorBase::InputsSet);

        // tester.set_inputs_vector({{StubInput(43)}});  // This will assert-fail.

        // Also ok to call in this phase.
        tester.gp1.set(2.f);

        // Verify that 8-bit non-boolean GP values are parsed as integers, not chars.
        tester.gp_int8.set_from_string("68");
        tester.gp_uint8.set_from_string("67");
        tester.gp_char.set_from_string("107");
        tester.gp_schar.set_from_string("108");
        tester.gp_uchar.set_from_string("109");
        tester.gp_bool.set_from_string("false");

        tester.build_pipeline();
        internal_assert(tester.phase == GeneratorBase::ScheduleCalled);

        // tester.set_inputs_vector({{StubInput(45)}});  // This will assert-fail.
        // tester.gp2.set(2);  // This will assert-fail.
        // tester.sp2.set(202);  // This will assert-fail.
    }

    // Verify that set_inputs() works properly, even if the specific subtype of Generator is not known.
    {
        class Tester : public Generator<Tester> {
        public:
            Input<int> input_int{"input_int"};
            Input<float> input_float{"input_float"};
            Input<uint8_t> input_byte{"input_byte"};
            Input<uint64_t[4]> input_scalar_array{ "input_scalar_array" };
            Input<Func> input_func_typed{"input_func_typed", Int(16), 1};
            Input<Func> input_func_untyped{"input_func_untyped", 1};
            Input<Func[]> input_func_array{ "input_func_array", 1 };
            Input<Buffer<uint8_t>> input_buffer_typed{ "input_buffer_typed", 3 };
            Input<Buffer<>> input_buffer_untyped{ "input_buffer_untyped" };
            Output<Func> output{"output", Float(32), 1};


            void generate() {
                Var x;
                output(x) = input_int +
                            input_float +
                            input_byte +
                            input_scalar_array[3] +
                            input_func_untyped(x) +
                            input_func_typed(x) +
                            input_func_array[0](x) +
                            input_buffer_typed(x, 0, 0) +
                            input_buffer_untyped(x, Halide::_);
            }
            void schedule() {
                // nothing
            }
        };

        Tester tester_instance;
        tester_instance.init_from_context(context);
        // Use a base-typed reference to verify the code below doesn't know about subtype
        GeneratorBase &tester = tester_instance;

        const int i = 1234;
        const float f = 2.25f;
        const uint8_t b = 0x42;
        const std::vector<uint64_t> a = { 1, 2, 3, 4 };
        Var x;
        Func fn_typed, fn_untyped;
        fn_typed(x) = cast<int16_t>(38);
        fn_untyped(x) = 32.f;
        const std::vector<Func> fn_array = { fn_untyped, fn_untyped };

        Buffer<uint8_t> buf_typed(1, 1, 1);
        Buffer<float> buf_untyped(1);

        buf_typed.fill(33);
        buf_untyped.fill(34);

        // set_inputs() requires inputs in Input<>-decl-order,
        // and all inputs match type exactly.
        tester.set_inputs(i, f, b, a, fn_typed, fn_untyped, fn_array, buf_typed, buf_untyped);
        tester.call_generate();
        tester.call_schedule();

        Buffer<float> im = tester_instance.realize(1);
        internal_assert(im.dim(0).extent() == 1);
        internal_assert(im(0) == 1475.25f) << "Expected 1475.25 but saw " << im(0);
    }

    // Verify that array inputs and outputs are typed correctly.
    {
        class Tester : public Generator<Tester> {
        public:
            Input<int[]> expr_array_input{ "expr_array_input" };
            Input<Func[]> func_array_input{ "input_func_array" };
            Input<Buffer<>[]> buffer_array_input{ "buffer_array_input" };

            Input<int[]> expr_array_output{ "expr_array_output" };
            Output<Func[]> func_array_output{"func_array_output"};
            Output<Buffer<>[]> buffer_array_output{ "buffer_array_output" };


            void generate() {
            }
        };

        Tester tester_instance;

        static_assert(std::is_same<decltype(tester_instance.expr_array_input[0]), const Expr &>::value, "type mismatch");
        static_assert(std::is_same<decltype(tester_instance.expr_array_output[0]), const Expr &>::value, "type mismatch");

        static_assert(std::is_same<decltype(tester_instance.func_array_input[0]), const Func &>::value, "type mismatch");
        static_assert(std::is_same<decltype(tester_instance.func_array_output[0]), Func &>::value, "type mismatch");

        static_assert(std::is_same<decltype(tester_instance.buffer_array_input[0]), ImageParam>::value, "type mismatch");
        static_assert(std::is_same<decltype(tester_instance.buffer_array_output[0]), const Func &>::value, "type mismatch");
    }

    class GPTester : public Generator<GPTester> {
    public:
        GeneratorParam<int> gp{"gp", 0};
        Output<Func> output{"output", Int(32), 0};
        void generate() { output() = 0; }
        void schedule() {}
    };
    GPTester gp_tester;
    gp_tester.init_from_context(context);
    // Accessing the GeneratorParam will assert-fail if we
    // don't do some minimal setup here.
    gp_tester.set_inputs_vector({});
    gp_tester.call_generate();
    gp_tester.call_schedule();
    auto &gp = gp_tester.gp;


    // Verify that RDom parameter-pack variants can convert GeneratorParam to Expr
    RDom rdom(0, gp, 0, gp);

    // Verify that Func parameter-pack variants can convert GeneratorParam to Expr
    Var x, y;
    Func f, g;
    f(x, y) = x + y;
    g(x, y) = f(gp, gp);                            // check Func::operator() overloads
    g(rdom.x, rdom.y) += f(rdom.x, rdom.y);
    g.update(0).reorder(rdom.y, rdom.x);            // check Func::reorder() overloads for RDom::operator RVar()

    // Verify that print() parameter-pack variants can convert GeneratorParam to Expr
    print(f(0, 0), g(1, 1), gp);
    print_when(true, f(0, 0), g(1, 1), gp);

    // Verify that Tuple parameter-pack variants can convert GeneratorParam to Expr
    Tuple t(gp, gp, gp);

    std::cout << "Generator test passed" << std::endl;
}

}  // namespace Internal
}  // namespace Halide
