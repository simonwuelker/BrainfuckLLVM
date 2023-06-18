#include <fstream>
#include <iostream>
#include <vector>
#include <map>
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"

using namespace llvm;
using namespace std;

const unsigned int TAPE_SIZE = 0x4000;

static std::unique_ptr<LLVMContext> TheContext;
static std::unique_ptr<Module> TheModule;
static std::unique_ptr<IRBuilder<>> Builder;
static std::map<std::string, AllocaInst *> NamedValues;
static std::unique_ptr<legacy::FunctionPassManager> TheFPM;

static void llvm_init() {
    // Open a new module.
    TheContext = std::make_unique<LLVMContext>();
    TheModule = std::make_unique<Module>("brainfuck", *TheContext);

    // Create a new builder for the module.
    Builder = std::make_unique<IRBuilder<>>(*TheContext);

    // Create a new pass manager attached to it.
    TheFPM = std::make_unique<legacy::FunctionPassManager>(TheModule.get());

    // Do simple "peephole" optimizations and bit-twiddling optzns.
    TheFPM->add(createInstructionCombiningPass());
    // Reassociate expressions.
    TheFPM->add(createReassociatePass());
    // Eliminate Common SubExpressions.
    TheFPM->add(createGVNPass());
    // Simplify the control flow graph (deleting unreachable blocks, etc).
    TheFPM->add(createCFGSimplificationPass());

    TheFPM->doInitialization();

    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    InitializeNativeTargetAsmParser();
}

static Value* get_current_position() {
    AllocaInst *position_var = NamedValues["position"];
    return Builder->CreateLoad(
        position_var->getAllocatedType(), 
        position_var, 
        "position"
    );
}

static Value* get_current_tape_cell_ptr() {
    AllocaInst *tape = NamedValues["tape"];

    // Get the address of the cell value at the current positin
    return Builder->CreateGEP(
        tape->getAllocatedType(), 
        tape, 
        {
            ConstantInt::get(Type::getInt8Ty(*TheContext), 0), 
            get_current_position()
        },
        "tape cell ptr"
    );
}

static Value* get_current_tape_value() {
    Value* ptr = get_current_tape_cell_ptr();
    return Builder->CreateLoad(
        Type::getInt8Ty(*TheContext), 
        ptr
    );
}

namespace Ast {

class Node {
public:
    static Node* try_parse(std::fstream&);

    virtual void debug_print()=0;
    virtual void codegen()=0;

};

// +
class IncrementNode: public Node {
    void debug_print() override {
        std::cout << "+";
    }

    void codegen() override {
        // Load the current value in the cell
        Value* tape_cell_ptr = get_current_tape_cell_ptr();
        Value* tape_cell = Builder->CreateLoad(
            Type::getInt8Ty(*TheContext),
            tape_cell_ptr,
            "tape cell"
        );

        // Increment the value
        Value *to_add = ConstantInt::get(Type::getInt8Ty(*TheContext), 1);
        Value *new_value = Builder->CreateAdd(tape_cell, to_add, "new tape value");

        // Write back
        Builder->CreateStore(new_value, tape_cell_ptr);
    };
};

// -
class DecrementNode: public Node {
    void debug_print() override {
        std::cout << "-";
    }

    void codegen() override {
        // Load the current value in the cell
        Value* tape_cell_ptr = get_current_tape_cell_ptr();
        Value* tape_cell = Builder->CreateLoad(
            Type::getInt8Ty(*TheContext),
            tape_cell_ptr,
            "tape cell"
        );

        // Decrement the value
        Value *to_sub = ConstantInt::get(Type::getInt8Ty(*TheContext), 1);
        Value *new_value = Builder->CreateSub(tape_cell, to_sub, "new tape value");

        // Write back
        Builder->CreateStore(new_value, tape_cell_ptr);
    };
};

// <
class MoveLeftNode: public Node {
    void debug_print() override {
        std::cout << "<";
    }

    void codegen() override {
        AllocaInst *var = NamedValues["position"];

        Value *to_sub = ConstantInt::get(Type::getInt64Ty(*TheContext), 1);
        Value *current_value = Builder->CreateLoad(var->getAllocatedType(), var, "position");
        Value *new_value = Builder->CreateSub(current_value, to_sub, "next position");

        Builder->CreateStore(new_value, var);
    };
};

// > 
class MoveRightNode: public Node {
    void debug_print() override {
        std::cout << ">";
    }

    void codegen() override {
        AllocaInst *var = NamedValues["position"];

        Value *to_add = ConstantInt::get(Type::getInt64Ty(*TheContext), 1);
        Value *current_value = Builder->CreateLoad(var->getAllocatedType(), var, "position");
        Value *new_value = Builder->CreateAdd(current_value, to_add, "next position");

        Builder->CreateStore(new_value, var);
    };
};

// .
class PutCharNode: public Node {
    void debug_print() override {
        std::cout << ".";
    }

    void codegen() override {
        // declare putchar() function
        FunctionType* putchar_type = FunctionType::get(
            Type::getInt32Ty(*TheContext),  // returns int
            { Type::getInt8Ty(*TheContext) },  // single character argument
            false
        );
        FunctionCallee putchar = TheModule->getOrInsertFunction("putchar", putchar_type);

        // Read the cell value at the current position
        Value *tape_cell = get_current_tape_value();

        // Call putchar
        Builder->CreateCall(
            putchar_type, 
            putchar.getCallee(), 
            { tape_cell }, 
            "putchar()"
        );
    };
};

// ,
class GetCharNode: public Node {
    void debug_print() override {
        std::cout << ",";
    }

    void codegen() override {
        // declare getchar() function
        FunctionType* getchar_type = FunctionType::get(
            Type::getInt32Ty(*TheContext),
            {},
            false
        );
        FunctionCallee getchar = TheModule->getOrInsertFunction("getchar", getchar_type);

        // Call getchar
        Value *c = Builder->CreateCall(
            getchar_type, 
            getchar.getCallee(), 
            {}, 
            "getchar()"
        );

        // Truncate the value to an i8, so we can store it in the tape cell
        Value *truncated_char = Builder->CreateIntCast(c, Type::getInt8Ty(*TheContext), true);

        // Read the cell value at the current position
        AllocaInst *tape = NamedValues["tape"];
        Value *tape_cell_ptr = get_current_tape_cell_ptr();
        Builder->CreateStore(truncated_char, tape_cell_ptr);
    };
};

class ScopeNode: public Node {
protected:
    std::vector<Node *> children;

public:
    explicit ScopeNode(std::vector<Node *> children): children(children) {};

};

class ProgramNode: public ScopeNode {
public:
    using ScopeNode::ScopeNode;

    static Ast::Node* try_parse(std::fstream&);

    void debug_print() override {
        for (auto child: children) {
            child->debug_print();
        }
    }

    void codegen() override {
        // Setup main function
        FunctionType *main_type = FunctionType::get(Builder->getVoidTy(), false);
        Function* main = Function::Create(
            main_type, 
            GlobalValue::ExternalLinkage, 
            "main", 
            *TheModule
        );

        // Point the builder to the start of the main function
        BasicBlock *main_block = BasicBlock::Create(*TheContext, "entry", main);
        Builder->SetInsertPoint(main_block);

        // Allocate the position of the write head
        AllocaInst* position = Builder->CreateAlloca(
            Type::getInt64Ty(*TheContext),
            nullptr,
            "position"
        );

        Value* initial_value = ConstantInt::get(Type::getInt64Ty(*TheContext), 0);
        Builder->CreateStore(initial_value, position);
        NamedValues["position"] = position;

        // Allocate the tape storage
        Type* tape_type = ArrayType::get(Type::getInt8Ty(*TheContext), TAPE_SIZE);
        AllocaInst* tape = Builder->CreateAlloca(
            tape_type,
            nullptr,
            "tape"
        );

        Constant* initial_tape = ConstantAggregateZero::get(tape_type); 
        Builder->CreateStore(initial_tape, tape);
        NamedValues["tape"] = tape;

        // Emit IR for each child
        for (auto child: children) {
            child->codegen();
        }

        Builder->CreateRet(NULL);
        verifyFunction(*main);
    }
};

// [ ... ]
class ConditionalGroupNode: public ScopeNode {
public:
    using ScopeNode::ScopeNode;

    static Ast::Node* try_parse(std::fstream&);

    void debug_print() override {
        std::cout << '[';
        for (auto child: children) {
            child->debug_print();
        }
        std::cout << ']';
    }

    void codegen() override {
        AllocaInst *position_var = NamedValues["position"];
        AllocaInst *tape = NamedValues["tape"];

        BasicBlock *base_block = Builder->GetInsertBlock();
        Function *TheFunction = base_block->getParent();

        // Entry Condition:
        // Check if the current tape cell is zero, if so,
        // jump past the end of the group
        Value* start_condition = Builder->CreateICmpNE(
            get_current_tape_value(), 
            ConstantInt::get(Type::getInt8Ty(*TheContext), 0)
        );

        BasicBlock *group_content = BasicBlock::Create(*TheContext, "group content", TheFunction);
        BasicBlock *merge = BasicBlock::Create(*TheContext, "merge", TheFunction);

        Builder->CreateCondBr(start_condition, group_content, merge);

        // If the value is not zero, we simply emit all the child IR
        Builder->SetInsertPoint(group_content);

        for (auto child: children) {
            child->codegen();
        }

        // At the end of the is not zero block, chekc if the current
        // cell is zero, in which case jump back to the start of the group
        Value* end_condition = Builder->CreateICmpNE(
            get_current_tape_value(), 
            ConstantInt::get(Type::getInt8Ty(*TheContext), 0)
        );

        // Explicitly fallthrough out of the if branch
        Builder->CreateCondBr(end_condition, group_content, merge);

        // The merge block simply falls through back into the base block
        Builder->SetInsertPoint(merge);
    }
};
}

Ast::Node* Ast::Node::try_parse(fstream &in) {
    while(true) {
        char c;
        if (!(in >> c)) {
            return NULL;
        }
        
        switch (c) {
            case '+':
                return new Ast::IncrementNode();
            case '-':
                return new Ast::DecrementNode();
            case '<':
                return new Ast::MoveLeftNode();
            case '>':
                return new Ast::MoveRightNode();
            case '.':
                return new Ast::PutCharNode();
            case ',':
                return new Ast::GetCharNode();
            case '[':
                return Ast::ConditionalGroupNode::try_parse(in);
            case ']':
                return NULL; // don't continue parsing
            default:
                // Ignore all unknown characters
                break;
        }
    }
}

Ast::Node* Ast::ProgramNode::try_parse(fstream &in) {
    std::vector<Ast::Node *> children = {};

    Ast::Node* node;
    while ((node = Ast::Node::try_parse(in))) {
        children.push_back(node);
    }

    return new Ast::ProgramNode(std::move(children));
}

Ast::Node* Ast::ConditionalGroupNode::try_parse(fstream &in) {
    std::vector<Ast::Node *> children = {};

    Ast::Node* node;
    while ((node = Ast::Node::try_parse(in))) {
        children.push_back(node);
    }

    return new Ast::ConditionalGroupNode(std::move(children));
}

int main() {
    fstream in("program.bf");
    if (!in.is_open()) {
        std::cout << "Failed to open input file" << std::endl;
        return -1;
    }

    // build the AST
    Ast::Node *root = Ast::ProgramNode::try_parse(in);
    if (!root) {
        std::cout << "Failed to parse AST" << std::endl;
        return -1;
    }
    in.close();

    // Setup LLVM data structures
    llvm_init();

    // Emit LLVM IR code
    root->codegen();

    Function* main = TheModule->getFunction("main");
    if (!main) {
        std::cout << "main() was not defined" << std::endl;
        return -1;
    }

    // Optimize the function.
    TheFPM->run(*main);

    // Dump LLVM IR
    TheModule->print(outs(), nullptr);
    
    return 0;
}
