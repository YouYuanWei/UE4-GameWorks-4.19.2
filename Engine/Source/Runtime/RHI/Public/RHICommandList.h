// Copyright 1998-2014 Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	RHICommandList.h: RHI Command List definitions for queueing up & executing later.
=============================================================================*/

#pragma once

enum ERHICommandType
{
	// Start the enum at non-zero to help debug
	ERCT_NopBlob = 1000,	// NoOp, maybe has a buffer to be auto alloc/freed
	ERCT_SetRasterizerState,
	ERCT_SetShaderParameter,
	ERCT_SetShaderUniformBuffer,
	ERCT_SetShaderTexture,
	ERCT_SetShaderSampler,
	ERCT_DrawPrimitive,
	ERCT_DrawIndexedPrimitive,
	ERCT_SetBoundShaderState,
	ERCT_SetBlendState,
	ERCT_SetStreamSource,

	ERCT_,
};

class RHI_API FRHICommandList
{
	static FRHICommandList NullRHICommandList;
public:
	enum
	{
		DefaultMemSize = 256 * 1024,
		Alignment = MIN_ALIGNMENT,
	};

	static FORCEINLINE FRHICommandList& GetNullRef()
	{
		return NullRHICommandList;
	}

	FRHICommandList(int32 Size = DefaultMemSize)
	{
		MemSize = Size;
		Memory = nullptr;
		if (Size)
		{
			Memory = (uint8*)FMemory::Malloc(MemSize, MIN_ALIGNMENT);
		}
		Reset();
	}
	~FRHICommandList()
	{
		if (!IsNull())
		{
			FMemory::Free(Memory);
		}
		MemSize = 0;
		Memory = (uint8*)0x1; // we want this to not bypass the cmd list; please just crash.
	}

	template <typename TShaderRHIParamRef>
	inline void SetShaderUniformBuffer(TShaderRHIParamRef Shader, uint32 BaseIndex, FUniformBufferRHIParamRef UniformBufferRHI);

	template <typename TShaderRHIParamRef>
	inline void SetShaderResourceViewParameter(TShaderRHIParamRef Shader, uint32 SamplerIndex, FShaderResourceViewRHIParamRef SRV);

	template <typename TShaderRHIParamRef>
	inline void SetShaderParameter(TShaderRHIParamRef Shader, uint32 BufferIndex, uint32 BaseIndex, uint32 NumBytes, const void* Value, bool bValueInStack = false);

	template <typename TShaderRHIParamRef>
	inline void SetShaderTexture(TShaderRHIParamRef Shader, uint32 TextureIndex, FTextureRHIParamRef Texture);

	template <typename TShaderRHIParamRef>
	inline void SetShaderSampler(TShaderRHIParamRef Shader, uint32 SamplerIndex, FSamplerStateRHIParamRef State);

	inline void SetUAVParameter(FComputeShaderRHIParamRef Shader, uint32 UAVIndex, FUnorderedAccessViewRHIParamRef UAV);
	inline void SetUAVParameter(FComputeShaderRHIParamRef Shader, uint32 UAVIndex, FUnorderedAccessViewRHIParamRef UAV, uint32 InitialCount);

	inline void SetBoundShaderState(FBoundShaderStateRHIParamRef BoundShaderState);

	inline void SetRasterizerState(FRasterizerStateRHIParamRef State);

	inline void SetBlendState(FBlendStateRHIParamRef State, const FLinearColor& BlendFactor);
	inline void SetBlendState(FBlendStateRHIParamRef State)
	{
		SetBlendState(State, FLinearColor::White);
	}

	inline void DrawPrimitive(uint32 PrimitiveType, uint32 BaseVertexIndex, uint32 NumPrimitives, uint32 NumInstances);
	inline void DrawIndexedPrimitive(FIndexBufferRHIParamRef IndexBuffer, uint32 PrimitiveType, int32 BaseVertexIndex, uint32 MinIndex, uint32 NumVertices, uint32 StartIndex, uint32 NumPrimitives, uint32 NumInstances);
	inline void SetStreamSource(uint32 StreamIndex, FVertexBufferRHIParamRef VertexBuffer, uint32 Stride, uint32 Offset);

	FORCEINLINE bool IsNull() const
	{
		return !Memory;
	}
	FORCEINLINE void CheckIsNull() const
	{
		check(IsNull());
	}


	uint8* Alloc(SIZE_T InSize)
	{
		checkSlow(!IsNull());
		InSize = (InSize + (Alignment - 1)) & ~(Alignment - 1);
		check(Tail + InSize + Alignment <= &Memory[MemSize]);
		uint8* New = Tail;
		Tail += InSize;
		return New;
	}

	const uint8* GetHead() const
	{
		checkSlow(!IsNull());
		return &Memory[0];
	}

	const uint8* GetTail() const
	{
		checkSlow(!IsNull());
		return Tail;
	}

protected:
	uint8* Memory;
	int32 MemSize;

	enum EState
	{
		Ready,
		Executing,
		Kicked,
	};
	EState State;
	uint8* Tail;
	uint32 NumCommands;

	inline bool CanAddCommand() const
	{
		return (State == Ready);
	}

	bool CanFitCommand(SIZE_T Size) const
	{
		return (Size + (Tail - Memory) <= (SIZE_T)MemSize);
	}

	template <typename TCmd>
	FORCEINLINE TCmd* AddCommand()
	{
		checkSlow(!IsNull());
		check(CanAddCommand());
		check(CanFitCommand(sizeof(TCmd)));
		TCmd* Cmd = (TCmd*)Alloc(sizeof(TCmd));
		++NumCommands;
		return Cmd;
	}

	void Reset()
	{
		State = Kicked;
		NumCommands = 0;
		Tail = Memory;
	}

	FORCEINLINE bool Bypass()
	{
		return IsNull();
	}

	friend class FRHICommandListExecutor;
};

class RHI_API FRHICommandListExecutor
{
public:
	FRHICommandListExecutor();
	FRHICommandList& CreateList();
	void ExecuteAndFreeList(FRHICommandList& CmdList);

	void Verify();

private:
	FCriticalSection CriticalSection;
	bool bLock;
};
extern RHI_API FRHICommandListExecutor GRHICommandList;

DECLARE_CYCLE_STAT_EXTERN(TEXT("RHI Command List Execute"),STAT_RHICmdListExecuteTime,STATGROUP_RHI, );
DECLARE_CYCLE_STAT_EXTERN(TEXT("RHI Command List Enqueue"),STAT_RHICmdListEnqueueTime,STATGROUP_RHI, );
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("RHI Command List memory"),STAT_RHICmdListMemory,STATGROUP_RHI,RHI_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("RHI Command List count"),STAT_RHICmdListCount,STATGROUP_RHI,RHI_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("RHI Counter TEMP"), STAT_RHICounterTEMP,STATGROUP_RHI,RHI_API);

#include "RHICommandList.inl"
