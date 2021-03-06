#ifdef _MSC_VER
#pragma warning(disable: 4146) // unary minus operator applied to unsigned type, result still unsigned
#endif

#include "Parser.h"

#pragma warning(push, 0)    

// Clang includes
#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/RecordLayout.h>
#include <clang/Analysis/CFG.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/LangOptions.h>
#include <clang/Basic/TargetInfo.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Tooling/CommonOptionsParser.h>
#include <clang/Tooling/Tooling.h>

// LLVM includes
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>
#include <iostream>

#pragma warning(pop)    

#include "LayoutDefinitions.h"
#include "IO.h"

namespace ClangParser 
{
    Layout::Node* g_queryResult = nullptr;
    Parser::LocationFilter g_locationFilter;

    namespace Helpers
    {
        inline bool IsMSLayout(const clang::ASTContext& context) { return context.getTargetInfo().getCXXABI().isMicrosoft(); }

        void DestroyTree(Layout::Node* node)
        { 
            if (node)
            { 
                for(Layout::Node* child : node->children) 
                { 
                    DestroyTree(child);
                } 

                delete node;
            }
        }

        Layout::Node* ComputeStruct(const clang::ASTContext& context, const clang::CXXRecordDecl* declaration, const bool includeVirtualBases = true)
        {
            Layout::Node* node = new Layout::Node();

            const clang::ASTRecordLayout& layout = context.getASTRecordLayout(declaration);

            //basic data
            node->type   = declaration->getQualifiedNameAsString();
            node->size   = layout.getSize().getQuantity(); 
            node->align  = layout.getAlignment().getQuantity();

            //Check for bases 

            const clang::CXXRecordDecl* primaryBase = layout.getPrimaryBase();

            if(declaration->isDynamicClass() && !primaryBase && !Helpers::IsMSLayout(context))
            {
                //vtable pointer
                Layout::Node* vPtrNode = new Layout::Node(); 
                vPtrNode->nature = Layout::Category::VTablePtr; 
                vPtrNode->offset = 0u; 
                vPtrNode->size   = context.toCharUnitsFromBits(context.getTargetInfo().getPointerWidth(0)).getQuantity(); 
                vPtrNode->align  = context.toCharUnitsFromBits(context.getTargetInfo().getPointerAlign(0)).getQuantity();
                node->children.push_back(vPtrNode);
            }
            else if(layout.hasOwnVFPtr())
            {
                //vftable pointer
                Layout::Node* vPtrNode = new Layout::Node();
                vPtrNode->nature = Layout::Category::VFTablePtr;
                vPtrNode->offset = 0u;
                vPtrNode->size   = context.toCharUnitsFromBits(context.getTargetInfo().getPointerWidth(0)).getQuantity();
                vPtrNode->align  = context.toCharUnitsFromBits(context.getTargetInfo().getPointerAlign(0)).getQuantity();
                node->children.push_back(vPtrNode);
            }

            //Collect nvbases
            clang::SmallVector<const clang::CXXRecordDecl *,4> bases;
            for(const clang::CXXBaseSpecifier &base : declaration->bases())
            {
                assert(!base.getType()->isDependentType() && "Cannot layout class with dependent bases.");

                if(!base.isVirtual())
                {
                    bases.push_back(base.getType()->getAsCXXRecordDecl());
                }
            }

            // Sort nvbases by offset.
            llvm::stable_sort(bases,[&](const clang::CXXRecordDecl* lhs,const clang::CXXRecordDecl* rhs){ return layout.getBaseClassOffset(lhs) < layout.getBaseClassOffset(rhs); });

            // compute nvbases
            for(const clang::CXXRecordDecl* base : bases)
            {
                Layout::Node* baseNode = ComputeStruct(context,base,false); 
                baseNode->offset = layout.getBaseClassOffset(base).getQuantity();
                baseNode->nature = base == primaryBase? Layout::Category::NVPrimaryBase : Layout::Category::NVBase;
                node->children.push_back(baseNode);
            }

            // vbptr (for Microsoft C++ ABI)
            if(layout.hasOwnVBPtr())
            {                
                //vbtable pointer
                Layout::Node* vPtrNode = new Layout::Node();
                vPtrNode->nature = Layout::Category::VBTablePtr;
                vPtrNode->offset = layout.getVBPtrOffset().getQuantity();
                vPtrNode->size   = context.getTargetInfo().getPointerWidth(0);
                vPtrNode->align  = context.getTargetInfo().getPointerAlign(0);
                node->children.push_back(vPtrNode);
            }

            //Check for fields 
            unsigned int fieldNo = 0;
            for(clang::RecordDecl::field_iterator I = declaration->field_begin(),E = declaration->field_end(); I != E; ++I,++fieldNo)
            {
                const clang::FieldDecl& field = **I;
                const uint64_t localFieldOffsetInBits = layout.getFieldOffset(fieldNo);
                const clang::CharUnits fieldOffset = context.toCharUnitsFromBits(localFieldOffsetInBits);

                // Recursively visit fields of record type.
                if (const clang::CXXRecordDecl* fieldDeclarationCXX = field.getType()->getAsCXXRecordDecl())
                {
                    Layout::Node* fieldNode = ComputeStruct(context,fieldDeclarationCXX,true);
                    fieldNode->name   = field.getNameAsString();
                    fieldNode->type   = field.getType().getAsString(); //check if this or qualified types form function is better
                    fieldNode->offset = fieldOffset.getQuantity();
                    fieldNode->nature = Layout::Category::ComplexField;
                    node->children.push_back(fieldNode);
                }
                else
                {
                    if(field.isBitField())
                    {
                        const clang::TypeInfo fieldInfo = context.getTypeInfo(field.getType());

                        //bitfield
                        Layout::Node* fieldNode = new Layout::Node();
                        fieldNode->name   = field.getNameAsString(); 
                        fieldNode->type   = field.getType().getAsString();

                        fieldNode->nature = Layout::Category::Bitfield;
                        fieldNode->offset = fieldOffset.getQuantity();
                        fieldNode->size   = context.toCharUnitsFromBits(fieldInfo.Width).getQuantity();
                        fieldNode->align  = context.toCharUnitsFromBits(fieldInfo.Align).getQuantity();

                        Layout::Node* extraData = new Layout::Node();
                        extraData->offset  = localFieldOffsetInBits - context.toBits(fieldOffset); 
                        extraData->size    = field.getBitWidthValue(context);
                        fieldNode->children.push_back(extraData);

                        node->children.push_back(fieldNode);
                    }
                    else
                    {
                        const clang::TypeInfo fieldInfo = context.getTypeInfo(field.getType());

                        //simple field
                        Layout::Node* fieldNode = new Layout::Node();
                        fieldNode->name   = field.getNameAsString(); 
                        fieldNode->type   = field.getType().getAsString();

                        fieldNode->nature = Layout::Category::SimpleField;
                        fieldNode->offset = fieldOffset.getQuantity();
                        fieldNode->size   = context.toCharUnitsFromBits(fieldInfo.Width).getQuantity();
                        fieldNode->align  = context.toCharUnitsFromBits(fieldInfo.Align).getQuantity();
                        node->children.push_back(fieldNode);
                    }
                }
            }

            //Virtual bases
            if(includeVirtualBases)
            {
                const clang::ASTRecordLayout::VBaseOffsetsMapTy &vtorDisps = layout.getVBaseOffsetsMap();
                for(const clang::CXXBaseSpecifier& Base : declaration->vbases())
                {
                    assert(Base.isVirtual() && "Found non-virtual class!");

                    const clang::CXXRecordDecl* vBase = Base.getType()->getAsCXXRecordDecl();
                    const clang::CharUnits vBaseOffset = layout.getVBaseClassOffset(vBase);

                    if(vtorDisps.find(vBase)->second.hasVtorDisp())
                    {
                        clang::CharUnits size = clang::CharUnits::fromQuantity(4);

                        Layout::Node* vtorDispNode = new Layout::Node();
                        vtorDispNode->nature = Layout::Category::VtorDisp;
                        vtorDispNode->offset = (vBaseOffset - size).getQuantity();
                        vtorDispNode->size   = size.getQuantity();
                        vtorDispNode->align  = size.getQuantity();
                        node->children.push_back(vtorDispNode);
                    }

                    Layout::Node* vBaseNode = ComputeStruct(context,vBase,false);
                    vBaseNode->offset = vBaseOffset.getQuantity();
                    vBaseNode->nature = vBase == primaryBase? Layout::Category::VPrimaryBase : Layout::Category::VBase;
                    node->children.push_back(vBaseNode);
                }
            }

            return node;
        }
    }

    /////////////////////////////////////////////////////////////////////////////////////////////////////////////
    class FindStructAtLocationVisitor : public clang::RecursiveASTVisitor<FindStructAtLocationVisitor> 
    {
    public:
        FindStructAtLocationVisitor(const clang::SourceManager& sourceManager)
            : m_sourceManager(sourceManager)
            , m_best(nullptr)
            , m_bestStartLine(0u)
            , m_bestStartCol(0u)
        {}

        bool VisitCXXRecordDecl(clang::CXXRecordDecl* declaration) 
        {
            TryRecord(declaration,declaration->getSourceRange());
            return true;
        }

        bool VisitVarDecl(clang::VarDecl* declaration) 
        {          
            TryRecord(declaration->getType()->getAsCXXRecordDecl(),declaration->getSourceRange());
            return true;
        }

        const clang::CXXRecordDecl* GetBest() const { return m_best; }

    private: 
        void TryRecord(const clang::CXXRecordDecl* declaration, const clang::SourceRange& sourceRange)
        { 
            if (declaration && ( declaration->isClass() || declaration->isStruct() ) && !declaration->isDependentType() && declaration->getDefinition() && !declaration->isInvalidDecl() && declaration->isCompleteDefinition())
            { 
                //Check range
                const clang::PresumedLoc startLocation = m_sourceManager.getPresumedLoc(sourceRange.getBegin());
                const clang::PresumedLoc endLocation = m_sourceManager.getPresumedLoc(sourceRange.getEnd());

                const unsigned int startLine = startLocation.getLine();
                const unsigned int startCol  = startLocation.getColumn();
                const unsigned int endLine   = endLocation.getLine();
                const unsigned int endCol    = endLocation.getColumn();
                
                if ( (g_locationFilter.row > startLine || (g_locationFilter.row == startLine && g_locationFilter.col >= startCol)) && 
                    (g_locationFilter.row < endLine    || (g_locationFilter.row == endLine   && g_locationFilter.col <= endCol))   &&
                    (startLine > m_bestStartLine || (startLine == m_bestStartLine && startCol > m_bestStartCol)))
                { 
                    m_best = declaration; 
                    m_bestStartLine = startLine;
                    m_bestStartCol  = startCol;
                }
            }
        }

    private:
        const clang::SourceManager& m_sourceManager;
        const clang::CXXRecordDecl* m_best;

        unsigned int m_bestStartLine;
        unsigned int m_bestStartCol; 
    };

    class Consumer : public clang::ASTConsumer 
    {
    public:
        virtual void HandleTranslationUnit(clang::ASTContext& context) override
        {
            const clang::SourceManager& sourceManager = context.getSourceManager();
            auto Decls = context.getTranslationUnitDecl()->decls();

            FindStructAtLocationVisitor visitor(sourceManager);
            for (auto& Decl : Decls) 
            {
                const auto& FileID = sourceManager.getFileID(Decl->getLocation());
                if (FileID == sourceManager.getMainFileID() && ContainsLocation(sourceManager,Decl->getSourceRange()))
                {
                    visitor.TraverseDecl(Decl);
                }
            }

            if (const clang::CXXRecordDecl* best = visitor.GetBest())
            {
                g_queryResult = Helpers::ComputeStruct(context, best);
            }
        }
    private: 

        bool ContainsLocation(const clang::SourceManager& sourceManager, const clang::SourceRange& sourceRange) const
        { 
            const clang::PresumedLoc startLocation = sourceManager.getPresumedLoc(sourceRange.getBegin());
            const clang::PresumedLoc endLocation = sourceManager.getPresumedLoc(sourceRange.getEnd());

            const unsigned int startLine = startLocation.getLine();
            const unsigned int startCol  = startLocation.getColumn();
            const unsigned int endLine   = endLocation.getLine();
            const unsigned int endCol    = endLocation.getColumn();

            return (g_locationFilter.row > startLine || (g_locationFilter.row == startLine && g_locationFilter.col >= startCol)) && 
                   (g_locationFilter.row < endLine   || (g_locationFilter.row == endLine   && g_locationFilter.col <= endCol));
        }
    };

    class Action : public clang::ASTFrontendAction 
    {
    public:
        using ASTConsumerPointer = std::unique_ptr<clang::ASTConsumer>;
        ASTConsumerPointer CreateASTConsumer(clang::CompilerInstance&, llvm::StringRef) override { return std::make_unique<Consumer>(); }
    };
}

namespace Parser
{ 
    void SetFilter(const LocationFilter& filter)
    { 
        ClangParser::g_locationFilter = filter;
    }
        
    void ConsoleLog(llvm::StringRef str)
    { 
        IO::Log(str.str().c_str());
    }

	bool Parse(const char* filename, int argc, const char* argv[])
	{ 
        llvm::outs().SetCustomConsole(&ConsoleLog);
        llvm::outs().SetUnbuffered();
        llvm::errs().SetCustomConsole(&ConsoleLog);
        llvm::errs().SetUnbuffered();
        
        //Parse command line 
        std::vector<std::string> SourcePaths; 
        SourcePaths.push_back(filename);

        std::unique_ptr<clang::tooling::CompilationDatabase> Compilations;
        std::string ErrorMessage;
        Compilations = clang::tooling::FixedCompilationDatabase::loadFromCommandLine(argc, argv, ErrorMessage);

        if (!ErrorMessage.empty()) 
        { 
            ErrorMessage.append("\n");
            llvm::errs() << ErrorMessage;
        }

        if (!Compilations) 
        {    
            Compilations = clang::tooling::CompilationDatabase::autoDetectFromSource(filename, ErrorMessage);
            
            if (!Compilations) 
            {
                llvm::errs() << "Error while trying to load a compilation database:\n" << ErrorMessage << "Running without flags.\n";
                Compilations.reset(new clang::tooling::FixedCompilationDatabase(".", std::vector<std::string>()));
            }
        }

        clang::tooling::ClangTool tool(*Compilations,SourcePaths);

        const int retCode = tool.run(clang::tooling::newFrontendActionFactory<ClangParser::Action>().get()); 
        return retCode == 0;
	}

	const Layout::Node* GetLayout()
	{ 
        return ClangParser::g_queryResult;
	}

    void Clear()
    { 
        ClangParser::Helpers::DestroyTree(ClangParser::g_queryResult);
        ClangParser::g_queryResult = nullptr;
    }
}