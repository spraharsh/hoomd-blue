#include "EvalFactory.h"
#include "ClangCompiler.h"

#include <memory>
#include <sstream>
#include <utility>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"

#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include "llvm/ExecutionEngine/Orc/OrcABISupport.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"

#include "llvm/Support/raw_os_ostream.h"

#pragma GCC diagnostic pop

//! C'tor
EvalFactory::EvalFactory(const std::string& cpp_code,
                         const std::vector<std::string>& compiler_args)
    {
    // set to null pointer
    m_eval = NULL;

    // initialize LLVM
    std::ostringstream sstream;

    auto clang_compiler = ClangCompiler::createClangCompiler();

    // Add the program's symbols into the JIT's search space.
    if (llvm::sys::DynamicLibrary::LoadLibraryPermanently(nullptr))
        {
        m_error_msg = "Error loading program symbols.\n";
        return;
        }

    llvm::LLVMContext Context;
    llvm::SMDiagnostic Err;

    // compile the module
    auto module = clang_compiler->compileCode(cpp_code, compiler_args, Context, sstream);

    if (!module)
        {
        // if the module didn't load, report an error
        m_error_msg = sstream.str();
        return;
        }

    // Build the JIT
    m_jit = llvm::orc::KaleidoscopeJIT::Create();

    if (!m_jit)
        {
        m_error_msg = "Could not initialize JIT.";
        return;
        }

    // Add the module.
    if (auto E = m_jit->addModule(std::move(module)))
        {
        m_error_msg = "Could not add JIT module.";
        return;
        }

    // Look up the eval function pointer.
    auto eval = m_jit->findSymbol("eval");

    if (!eval)
        {
        m_error_msg = "Could not find eval function in LLVM module.";
        return;
        }

    auto alpha = m_jit->findSymbol("param_array");

    if (!alpha)
        {
        m_error_msg = "Could not find alpha array in LLVM module.";
        return;
        }

    auto alpha_union = m_jit->findSymbol("alpha_union");

    if (!alpha_union)
        {
        m_error_msg = "Could not find alpha_union array in LLVM module.";
        return;
        }

    m_eval = (EvalFnPtr)(long unsigned int)(eval->getAddress());
    m_alpha = (float**)(alpha->getAddress());
    m_alpha_union = (float**)(alpha_union->getAddress());
    }
