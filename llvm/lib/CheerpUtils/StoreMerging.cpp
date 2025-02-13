//===-- StoreMerging.cpp - Merging of adjacent stores --------------------------===//
//
//                     Cheerp: The C++ compiler for the Web
//
// This file is distributed under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.TXT for details.
//
// Copyright 2014-2023 Leaning Technologies
//
//===----------------------------------------------------------------------===//

#include "llvm/InitializePasses.h"
#include "llvm/Cheerp/CommandLine.h"
#include "llvm/Cheerp/GlobalDepsAnalyzer.h"
#include "llvm/Cheerp/InvokeWrapping.h"
#include "llvm/Cheerp/Utility.h"
#include "llvm/Cheerp/StoreMerging.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

using namespace cheerp;

bool StoreMerging::runOnFunction(Function& F)
{
	bool Changed = false;

	for (BasicBlock& BB : F)
		Changed |= runOnBasicBlock(BB);

	return Changed;
}

bool StoreMerging::runOnBasicBlock(BasicBlock& BB)
{
	assert(DL);
	const bool asmjs = BB.getParent()->getSection() == StringRef("asmjs");

	if (!asmjs)
		return false;

	const llvm::Value* currentPtr = nullptr;
	std::vector<StoreAndOffset> basedOnCurrentPtr;

	bool Changed = false;

	for (Instruction& I : BB)
	{
		if (StoreInst* SI = dyn_cast<StoreInst>(&I))
		{
			auto pair = findBasePointerAndOffset(SI->getPointerOperand());

			if (currentPtr == nullptr)
				currentPtr = pair.first;

			if (currentPtr != pair.first)
			{
				Changed |= processBlockOfStores(basedOnCurrentPtr);
				basedOnCurrentPtr.clear();
			}

			currentPtr = pair.first;
			Type* storedType = SI->getValueOperand()->getType();
			basedOnCurrentPtr.emplace_back(SI, DL->getTypeAllocSize(storedType), pair.second, basedOnCurrentPtr.size());
			continue;
		}

		if (I.mayReadOrWriteMemory() || I.mayHaveSideEffects())
		{
			Changed |= processBlockOfStores(basedOnCurrentPtr);
			currentPtr = nullptr;
			basedOnCurrentPtr.clear();
		}
	}

	Changed |= processBlockOfStores(basedOnCurrentPtr);

	return Changed;
}

void StoreMerging::sortStores(std::vector<StoreAndOffset>& groupedSamePointer)
{
	// We find candidates for merging by scanning this vector and looking
	// at adjacent value. The order should be:
	// 1) By size, since we can only merge stores of the same size
	// 2) By offset, since we want adjacent values to be close to each other
	std::sort(groupedSamePointer.begin(), groupedSamePointer.end(),
		[](const StoreAndOffset& left, const StoreAndOffset& right) -> bool
		{
			return left.offset < right.offset;
		});

}

void StoreMerging::filterAlreadyProcessedStores(std::vector<StoreAndOffset>& groupedSamePointer)
{
	//Bookkeeping 3: remove the stores with size set to 0
	//We use a temporary vector, and then swap it out for the older one
	std::vector<StoreAndOffset> newGroupedSamePointer;

	for (uint32_t i=0; i<groupedSamePointer.size(); i++)
	{
		if (groupedSamePointer[i].size == 0)
			continue;
		newGroupedSamePointer.push_back(groupedSamePointer[i]);
	}

	std::swap(newGroupedSamePointer, groupedSamePointer);
}

bool StoreMerging::processBlockOfStores(std::vector<StoreAndOffset>& groupedSamePointer)
{
	if (groupedSamePointer.size() < 2)
		return false;

	sortStores(groupedSamePointer);
	const uint32_t N = groupedSamePointer.size();
	bool overlap = false;
	for (uint32_t i=0; i+1<N; i++)
	{
		if (groupedSamePointer[i].offset + groupedSamePointer[i].size > groupedSamePointer[i+1].offset)
			overlap = true;
	}


	//Avoid the optimization if any store overlap
	if (overlap)
		return false;
	bool Changed = false;

	//Alternatively process a block of stores and filter out already consumed ones
	//Processing with increasing size means that we may optimize even already optimized stores
	Changed |= processBlockOfStores(1, groupedSamePointer);
	filterAlreadyProcessedStores(groupedSamePointer);

	Changed |= processBlockOfStores(2, groupedSamePointer);
	filterAlreadyProcessedStores(groupedSamePointer);

	//Do not create 64-bit asmjs stores
	if (!isWasm)
		return Changed;

	Changed |= processBlockOfStores(4, groupedSamePointer);
	filterAlreadyProcessedStores(groupedSamePointer);
	return Changed;
}

bool StoreMerging::processBlockOfStores(const uint32_t dim, std::vector<StoreAndOffset> & groupedSamePointer)
{
	const uint32_t N = groupedSamePointer.size();

	bool Changed = false;

	for (uint32_t i=0; i+1<N; i++)
	{
		const uint32_t a = i;
		const uint32_t b = i+1;

		if (groupedSamePointer[a].size != dim)
			continue;
		if (groupedSamePointer[b].size != dim)
			continue;

		//Check they are consecutive
		if ((int)dim + groupedSamePointer[a].offset != groupedSamePointer[b].offset)
			continue;

		StoreInst* lowStore = groupedSamePointer[a].store;
		StoreInst* highStore = groupedSamePointer[b].store;

		const uint32_t alignment = lowStore->getAlign().value();

		if (!isWasm && alignment < dim * 2)
			continue;

		Value* lowValue = lowStore->getValueOperand();
		Value* highValue = highStore->getValueOperand();

		//For now avoid complexities related to float/double to int bitcasts
		if (lowValue->getType()->isFloatTy() || lowValue->getType()->isVectorTy())
			continue;
		if (highValue->getType()->isFloatTy() || highValue->getType()->isVectorTy())
			continue;

		const Constant* constantLowValue = dyn_cast<Constant>(lowValue);
		const Constant* constantHighValue = dyn_cast<Constant>(highValue);

		enum STRATEGY { NOT_CONVENIENT = 0, CONSTANT = 1, ZERO_EXTEND = 2};
		STRATEGY strategy = NOT_CONVENIENT;

		//Both ValueOperands constants -> folded in a single store
		if (constantLowValue && constantHighValue)
			strategy = CONSTANT;
		//Higher ValueOperands 0 -> folded in a single store
		else if (constantHighValue && constantHighValue->isNullValue())
			strategy = ZERO_EXTEND;

		if (strategy == NOT_CONVENIENT)
			continue;

		auto& context = lowStore->getParent()->getContext();
		Type* bigType = IntegerType::get(context, dim * 16);
		Type* int32Type = IntegerType::get(context, 32);

		// The insertion point will be the store writing to the first byte
		IRBuilder<> builder(lowStore);

		auto convertToBigType = [&builder, &bigType, &int32Type, &context, this](Value* value) -> Value*
		{
			//Convert to integer (either from pointer or other type)
			if (value->getType()->isPointerTy())
				value = builder.CreatePtrToInt(value, int32Type);
			else if (!value->getType()->isIntegerTy())
			{
				Type* integerEquivalent = IntegerType::get(context, DL->getTypeAllocSize(value->getType()));
				value = builder.CreateBitCast(value, integerEquivalent);
			}

			//Then zero extend
			if (value->getType() != bigType)
				value = builder.CreateZExt(value, bigType);

			return value;
		};

		Value* sum = nullptr;
		if(strategy == CONSTANT || strategy == ZERO_EXTEND)
		{
			sum = convertToBigType(lowValue);

			//Add shifted higher part
			if (strategy == CONSTANT)
			{
				Value* high = convertToBigType(highValue);
				Value* shiftToHigh = builder.CreateShl(high, dim*8);
				sum = builder.CreateAdd(sum, shiftToHigh);
			}
		}
		else
		{
			assert(false);
		}

		//BitCast the pointer operand
		Value* bitcast = builder.CreateBitCast(lowStore->getPointerOperand(), bigType->getPointerTo());

		//Actually create the store
		StoreInst* biggerStore = cast<StoreInst>(builder.CreateStore(sum, bitcast));
		biggerStore->setAlignment(llvm::Align(alignment));

		//Bookkeeping 1: erase used stores
		lowStore->eraseFromParent();
		highStore->eraseFromParent();

		//Bookkeeping 2: insert biggerStore at the right point in groupedSamePointer
		groupedSamePointer[a].store = biggerStore;
		groupedSamePointer[a].size = dim*2;
		groupedSamePointer[b].size = 0;

		i = b;

		Changed = true;
	}
	return Changed;
}

std::pair<const llvm::Value*, int> StoreMerging::findBasePointerAndOffset(const llvm::Value* pointer)
{
	int totalOffset = 0;

	while (true)
	{
		if (const GetElementPtrInst* gep = dyn_cast<GetElementPtrInst>(pointer))
		{
			if (gep->hasAllConstantIndices())
			{
				llvm::Type* curType = gep->getOperand(0)->getType();
				for(unsigned i=1; i<gep->getNumOperands(); i++)
				{
					ConstantInt* CI = dyn_cast<ConstantInt>(gep->getOperand(i));
					assert(CI);

					//partialOffset modifyies curType
					totalOffset += partialOffset(curType, gep->getSourceElementType(), *DL, CI->getSExtValue());
				}

				pointer = gep->getPointerOperand();
				continue;
			}
		}
		else if (const BitCastInst* bci = dyn_cast<BitCastInst>(pointer))
		{
			//totalOffset remains the same
			pointer = cast<BitCastInst>(pointer)->getOperand(0);
			continue;
		}

		break;
	}

	return {pointer, totalOffset};
}

llvm::PreservedAnalyses StoreMergingPass::run(Function& F, FunctionAnalysisManager& FAM)
{
	StoreMerging inner(F.getParent()->getDataLayout(), isWasm);
	if (!inner.runOnFunction(F))
		return PreservedAnalyses::all();

	PreservedAnalyses PA;
	PA.preserve<InvokeWrappingAnalysis>();
	PA.preserve<GlobalDepsAnalysis>();
	return PA;
}
