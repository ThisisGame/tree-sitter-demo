// Copyright Epic Games, Inc. All Rights Reserved.

#include "Widgets/SWidget.h"
#include "Types/PaintArgs.h"
#include "Layout/ArrangedChildren.h"
#include "Layout/Children.h"
#include "SlateGlobals.h"
#include "Rendering/DrawElements.h"
#include "Widgets/IToolTip.h"
#include "Misc/Paths.h"
#include "HAL/IConsoleManager.h"
#include "Types/NavigationMetaData.h"
#include "Application/SlateApplicationBase.h"
#include "Styling/CoreStyle.h"
#include "Application/ActiveTimerHandle.h"
#include "Input/HittestGrid.h"
#include "Debugging/SlateDebugging.h"
#include "Debugging/WidgetList.h"
#include "Widgets/SWindow.h"
#include "Trace/SlateTrace.h"
#include "Types/SlateCursorMetaData.h"
#include "Types/SlateMouseEventsMetaData.h"
#include "Types/ReflectionMetadata.h"
#include "Types/SlateToolTipMetaData.h"
#include "Stats/Stats.h"
#include "Containers/StringConv.h"
#include "Misc/ScopeRWLock.h"
#include "HAL/CriticalSection.h"
#include "Widgets/SWidgetUtils.h"

#if WITH_ACCESSIBILITY
#include "Widgets/Accessibility/SlateCoreAccessibleWidgets.h"
#include "Widgets/Accessibility/SlateAccessibleMessageHandler.h"
#endif

// Enabled to assign FindWidgetMetaData::FoundWidget to the widget that has the matching reflection data 
#ifndef UE_WITH_SLATE_DEBUG_FIND_WIDGET_REFLECTION_METADATA
	#define UE_WITH_SLATE_DEBUG_FIND_WIDGET_REFLECTION_METADATA 0
#endif

#if UE_WITH_SLATE_DEBUG_FIND_WIDGET_REFLECTION_METADATA
namespace FindWidgetMetaData
{
	SWidget* FoundWidget = nullptr;
	FName WidgeName = "ItemNameToFind";
	FName AssetName = "AssetNameToFind";
}
#endif

DEFINE_STAT(STAT_SlateTotalWidgetsPerFrame);
DEFINE_STAT(STAT_SlateNumPaintedWidgets);
DEFINE_STAT(STAT_SlateNumTickedWidgets);
DEFINE_STAT(STAT_SlateExecuteActiveTimers);
DEFINE_STAT(STAT_SlateTickWidgets);
DEFINE_STAT(STAT_SlatePrepass);
DEFINE_STAT(STAT_SlateTotalWidgets);
DEFINE_STAT(STAT_SlateSWidgetAllocSize);
DEFINE_STAT(STAT_SlateGetMetaData);

DECLARE_CYCLE_STAT(TEXT("SWidget::CreateStatID"), STAT_Slate_CreateStatID, STATGROUP_Slate);

template <typename AnnotationType>
class TWidgetSparseAnnotation
{
public:
	const AnnotationType* Find(const SWidget* Widget)
	{
		FRWScopeLock Lock(RWLock, SLT_ReadOnly);
		return AnnotationMap.Find(Widget);
	}

	AnnotationType& FindOrAdd(const SWidget* Widget)
	{
		FRWScopeLock Lock(RWLock, SLT_Write);
		return AnnotationMap.FindOrAdd(Widget);
	}

	void Add(const SWidget* Widget, const AnnotationType& Type)
	{
		FRWScopeLock Lock(RWLock, SLT_Write);
		AnnotationMap.Add(Widget, Type);
	}

	void Remove(const SWidget* Widget)
	{
		FRWScopeLock Lock(RWLock, SLT_Write);
		AnnotationMap.Remove(Widget);
	}
private:
	TMap<const SWidget*, AnnotationType> AnnotationMap;
	FRWLock RWLock;
};

#if WITH_ACCESSIBILITY
TWidgetSparseAnnotation<TAttribute<FText>> AccessibleText;
TWidgetSparseAnnotation<TAttribute<FText>> AccessibleSummaryText;
#endif

static void ClearSparseAnnotationsForWidget(const SWidget* Widget)
{
#if WITH_ACCESSIBILITY
	AccessibleText.Remove(Widget);
	AccessibleSummaryText.Remove(Widget);
#endif
}

#if SLATE_CULL_WIDGETS

float GCullingSlackFillPercent = 0.25f;
static FAutoConsoleVariableRef CVarCullingSlackFillPercent(TEXT("Slate.CullingSlackFillPercent"), GCullingSlackFillPercent, TEXT("Scales the culling rect by the amount to provide extra slack/wiggle room for widgets that have a true bounds larger than the root child widget in a container."), ECVF_Default);

#endif

#if WITH_SLATE_DEBUGGING

bool GShowClipping = false;
static FAutoConsoleVariableRef CVarSlateShowClipRects(TEXT("Slate.ShowClipping"), GShowClipping, TEXT("Controls whether we should render a clipping zone outline.  Yellow = Axis Scissor Rect Clipping (cheap).  Red = Stencil Clipping (expensive)."), ECVF_Default);

bool GDebugCulling = false;
static FAutoConsoleVariableRef CVarSlateDebugCulling(TEXT("Slate.DebugCulling"), GDebugCulling, TEXT("Controls whether we should ignore clip rects, and just use culling."), ECVF_Default);

bool GSlateEnsureAllVisibleWidgetsPaint = false;
static FAutoConsoleVariableRef CVarSlateEnsureAllVisibleWidgetsPaint(TEXT("Slate.EnsureAllVisibleWidgetsPaint"), GSlateEnsureAllVisibleWidgetsPaint, TEXT("Ensures that if a child widget is visible before OnPaint, that it was painted this frame after OnPaint, if still marked as visible.  Only works if we're on the FastPaintPath."), ECVF_Default);

#endif

#if STATS || ENABLE_STATNAMEDEVENTS

void SWidget::CreateStatID() const
{
	SCOPE_CYCLE_COUNTER(STAT_Slate_CreateStatID);

	const FString LongName = FReflectionMetaData::GetWidgetDebugInfo(this);

#if STATS
	StatID = FDynamicStats::CreateStatId<FStatGroup_STATGROUP_Slate>(LongName);
#else // ENABLE_STATNAMEDEVENTS
	const auto& ConversionData = StringCast<PROFILER_CHAR>(*LongName);
	const int32 NumStorageChars = (ConversionData.Length() + 1);	//length doesn't include null terminator

	PROFILER_CHAR* StoragePtr = new PROFILER_CHAR[NumStorageChars];
	FMemory::Memcpy(StoragePtr, ConversionData.Get(), NumStorageChars * sizeof(PROFILER_CHAR));

	if (FPlatformAtomics::InterlockedCompareExchangePointer((void**)&StatIDStringStorage, StoragePtr, nullptr) != nullptr)
	{
		delete[] StoragePtr;
	}
	
	StatID = TStatId(StatIDStringStorage);
#endif
}

#endif

/**
 * 在SWidget::Paint()中调用，在OnPaint()之后，此时LayerID已经确定。
 * @param NewLayerId 
 * @param CacheHandle 
 */
void SWidget::UpdateWidgetProxy(int32 NewLayerId, FSlateCachedElementsHandle& CacheHandle)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(SWidget::UpdateWidgetProxy);

#if WITH_SLATE_DEBUGGING
	check(!CacheHandle.IsValid() || CacheHandle.IsOwnedByWidget(this));
#endif

	// Account for the case when the widget gets a new handle for some reason.  This should really never happen
	if (PersistentState.CachedElementHandle.IsValid() && PersistentState.CachedElementHandle != CacheHandle)
	{
		ensureMsgf(!CacheHandle.IsValid(), TEXT("Widget was assigned a new cache handle.  This is not expected to happen."));
		PersistentState.CachedElementHandle.RemoveFromCache();
	}
	PersistentState.CachedElementHandle = CacheHandle;

	if (FastPathProxyHandle.IsValid(this))
	{
		FWidgetProxy& MyProxy = FastPathProxyHandle.GetProxy();

		MyProxy.Visibility = GetVisibility();

		//PersistentState.OutgoingLayerId = NewLayerId;

		if ((IsVolatile() && !IsVolatileIndirectly()) || (Advanced_IsInvalidationRoot() && !Advanced_IsWindow()))// 检查SWidget是否是易变的或者是否是无效化根
		{
			AddUpdateFlags(EWidgetUpdateFlags::NeedsVolatilePaint);
		}
		else
		{
			RemoveUpdateFlags(EWidgetUpdateFlags::NeedsVolatilePaint);
		}
		// 标记FastPathProxyHandle指向的FWidgetProxy已经在这一帧更新过
		FastPathProxyHandle.MarkWidgetUpdatedThisFrame();
	}
}

#if UE_SLATE_WITH_WIDGET_UNIQUE_IDENTIFIER
namespace SlateTraceMetaData
{
	uint64 UniqueIdGenerator = 0;
}
#endif

SWidget::SWidget()
	: bIsHovered(false)
	, bCanSupportFocus(true)
	, bCanHaveChildren(true)
	, bClippingProxy(false)
	, bToolTipForceFieldEnabled(false)
	, bForceVolatile(false)
	, bCachedVolatile(false)
	, bInheritedVolatility(false)
	, bInvisibleDueToParentOrSelfVisibility(false)
	, bNeedsPrepass(true)
	, bUpdatingDesiredSize(false)
	, bHasCustomPrepass(false)
	, bHasRelativeLayoutScale(false)
	, bVolatilityAlwaysInvalidatesPrepass(false)
#if WITH_ACCESSIBILITY
	, bCanChildrenBeAccessible(true)
	, AccessibleBehavior(EAccessibleBehavior::NotAccessible)
	, AccessibleSummaryBehavior(EAccessibleBehavior::Auto)
#endif
	, Clipping(EWidgetClipping::Inherit)
	, FlowDirectionPreference(EFlowDirectionPreference::Inherit)
	// Note we are defaulting to tick for backwards compatibility
	, UpdateFlags(EWidgetUpdateFlags::NeedsTick)
	, DesiredSize()
	, CullingBoundsExtension()
	, EnabledState(true)
	, Visibility(EVisibility::Visible)
	, RenderOpacity(1.0f)
	, RenderTransform()
	, RenderTransformPivot(FVector2D::ZeroVector)
#if UE_SLATE_WITH_WIDGET_UNIQUE_IDENTIFIER
	, UniqueIdentifier(++SlateTraceMetaData::UniqueIdGenerator)
#endif
#if ENABLE_STATNAMEDEVENTS
	, StatIDStringStorage(nullptr)
#endif
{
	if (GIsRunning)
	{
		INC_DWORD_STAT(STAT_SlateTotalWidgets);
		INC_DWORD_STAT(STAT_SlateTotalWidgetsPerFrame);
	}

	UE_SLATE_DEBUG_WIDGETLIST_ADD_WIDGET(this);
	UE_TRACE_SLATE_WIDGET_ADDED(this);
}

PRAGMA_DISABLE_DEPRECATION_WARNINGS
SWidget::~SWidget()
{
#if UE_WITH_SLATE_DEBUG_FIND_WIDGET_REFLECTION_METADATA
	if (FindWidgetMetaData::FoundWidget == this)
	{
		FindWidgetMetaData::FoundWidget = nullptr;
	}
#endif

	// Unregister all ActiveTimers so they aren't left stranded in the Application's list.
	if (FSlateApplicationBase::IsInitialized())
	{
		for (const auto& ActiveTimerHandle : ActiveTimers)
		{
			FSlateApplicationBase::Get().UnRegisterActiveTimer(ActiveTimerHandle);
		}

		// Warn the invalidation root
		if (FSlateInvalidationRoot* InvalidationRoot = FastPathProxyHandle.GetInvalidationRootHandle().GetInvalidationRoot())
		{
			InvalidationRoot->OnWidgetDestroyed(this);
		}

		// Reset handle
		FastPathProxyHandle = FWidgetProxyHandle();

		// Note: this would still be valid if a widget was painted and then destroyed in the same frame.  
		// In that case invalidation hasn't taken place for added widgets so the invalidation panel doesn't know about their cached element data to clean it up
		PersistentState.CachedElementHandle.RemoveFromCache();

#if WITH_ACCESSIBILITY
		FSlateApplicationBase::Get().GetAccessibleMessageHandler()->OnWidgetRemoved(this);
#endif
		// Only clear if initialized because SNullWidget's destructor may be called after annotations are deleted
		ClearSparseAnnotationsForWidget(this);
	}

#if ENABLE_STATNAMEDEVENTS
	delete[] StatIDStringStorage;
	StatIDStringStorage = nullptr;
#endif

	UE_SLATE_DEBUG_WIDGETLIST_REMOVE_WIDGET(this);
	UE_TRACE_SLATE_WIDGET_REMOVED(this);
	DEC_DWORD_STAT(STAT_SlateTotalWidgets);
	DEC_MEMORY_STAT_BY(STAT_SlateSWidgetAllocSize, AllocSize);
}
PRAGMA_ENABLE_DEPRECATION_WARNINGS

PRAGMA_DISABLE_DEPRECATION_WARNINGS
void SWidget::Construct(
	const TAttribute<FText>& InToolTipText,
	const TSharedPtr<IToolTip>& InToolTip,
	const TAttribute< TOptional<EMouseCursor::Type> >& InCursor,
	const TAttribute<bool>& InEnabledState,
	const TAttribute<EVisibility>& InVisibility,
	const float InRenderOpacity,
	const TAttribute<TOptional<FSlateRenderTransform>>& InTransform,
	const TAttribute<FVector2D>& InTransformPivot,
	const FName& InTag,
	const bool InForceVolatile,
	const EWidgetClipping InClipping,
	const EFlowDirectionPreference InFlowPreference,
	const TOptional<FAccessibleWidgetData>& InAccessibleData,
	const TArray<TSharedRef<ISlateMetaData>>& InMetaData
)
{
	FSlateBaseNamedArgs Args;
	Args._ToolTipText = InToolTipText;
	Args._ToolTip = InToolTip;
	Args._Cursor = InCursor;
	Args._IsEnabled = InEnabledState;
	Args._Visibility = InVisibility;
	Args._RenderOpacity = InRenderOpacity;
	Args._ForceVolatile = InForceVolatile;
	Args._Clipping = InClipping;
	Args._FlowDirectionPreference = InFlowPreference;
	Args._RenderTransform = InTransform;
	Args._RenderTransformPivot = InTransformPivot;
	Args._Tag = InTag;
	Args._AccessibleParams = InAccessibleData;
	Args.MetaData = InMetaData;
	SWidgetConstruct(Args);
}
PRAGMA_ENABLE_DEPRECATION_WARNINGS

PRAGMA_DISABLE_DEPRECATION_WARNINGS
void SWidget::SWidgetConstruct(const TAttribute<FText>& InToolTipText, const TSharedPtr<IToolTip>& InToolTip, const TAttribute< TOptional<EMouseCursor::Type> >& InCursor, const TAttribute<bool>& InEnabledState,
							   const TAttribute<EVisibility>& InVisibility, const float InRenderOpacity, const TAttribute<TOptional<FSlateRenderTransform>>& InTransform, const TAttribute<FVector2D>& InTransformPivot,
							   const FName& InTag, const bool InForceVolatile, const EWidgetClipping InClipping, const EFlowDirectionPreference InFlowPreference, const TOptional<FAccessibleWidgetData>& InAccessibleData,
							   const TArray<TSharedRef<ISlateMetaData>>& InMetaData)
{
	Construct(InToolTipText, InToolTip, InCursor, InEnabledState, InVisibility, InRenderOpacity, InTransform, InTransformPivot, InTag, InForceVolatile, InClipping, InFlowPreference, InAccessibleData, InMetaData);
}
PRAGMA_ENABLE_DEPRECATION_WARNINGS

void SWidget::SWidgetConstruct(const FSlateBaseNamedArgs& Args)
{
	EnabledState = Args._IsEnabled;
	Visibility = Args._Visibility;
	RenderOpacity = Args._RenderOpacity;
	RenderTransform = Args._RenderTransform;
	RenderTransformPivot = Args._RenderTransformPivot;

	// 因为在启用Trace时，在构造函数设置了Tag，所以这里判断新的Tag为空，就保留现有Trace的Tag。
#ifdef UE_TRACE_ENABLED
	if(Args._Tag.IsNone()==false)
	{
		Tag = Args._Tag;
	}
#else
	Tag = Args._Tag;
#endif
	bForceVolatile = Args._ForceVolatile;
	Clipping = Args._Clipping;
	FlowDirectionPreference = Args._FlowDirectionPreference;
	MetaData.Append(Args.MetaData);

	if (Args._ToolTip.IsSet())
	{
		// If someone specified a fancy widget tooltip, use it.
		SetToolTip(Args._ToolTip);
	}
	else if (Args._ToolTipText.IsSet())
	{
		// If someone specified a text binding, make a tooltip out of it
		SetToolTipText(Args._ToolTipText);
	}

	SetCursor(Args._Cursor);

#if WITH_ACCESSIBILITY
	// If custom text is provided, force behavior to custom. Otherwise, use the passed-in behavior and set their default text.
	if (Args._AccessibleText.IsSet() || Args._AccessibleParams.IsSet())
	{
		auto SetAccessibleWidgetData = [this](const FAccessibleWidgetData& AccessibleParams)
		{
			SetCanChildrenBeAccessible(AccessibleParams.bCanChildrenBeAccessible);
			SetAccessibleBehavior(AccessibleParams.AccessibleText.IsSet() ? EAccessibleBehavior::Custom : AccessibleParams.AccessibleBehavior, AccessibleParams.AccessibleText, EAccessibleType::Main);
		SetAccessibleBehavior(AccessibleParams.AccessibleSummaryText.IsSet() ? EAccessibleBehavior::Custom : AccessibleParams.AccessibleSummaryBehavior, AccessibleParams.AccessibleSummaryText, EAccessibleType::Summary);
		};
		if (Args._AccessibleText.IsSet())
		{
			SetAccessibleWidgetData(FAccessibleWidgetData{ Args._AccessibleText });
		}
		else
		{
			SetAccessibleWidgetData(Args._AccessibleParams.GetValue());
		}
	}
#endif
}

FReply SWidget::OnFocusReceived(const FGeometry& MyGeometry, const FFocusEvent& InFocusEvent)
{
	return FReply::Unhandled();
}

void SWidget::OnFocusLost(const FFocusEvent& InFocusEvent)
{
}

void SWidget::OnFocusChanging(const FWeakWidgetPath& PreviousFocusPath, const FWidgetPath& NewWidgetPath)
{
}

void SWidget::OnFocusChanging(const FWeakWidgetPath& PreviousFocusPath, const FWidgetPath& NewWidgetPath, const FFocusEvent& InFocusEvent)
{
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	OnFocusChanging(PreviousFocusPath, NewWidgetPath);
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
}

FReply SWidget::OnKeyChar( const FGeometry& MyGeometry, const FCharacterEvent& InCharacterEvent )
{
	return FReply::Unhandled();
}

FReply SWidget::OnPreviewKeyDown( const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent )
{
	return FReply::Unhandled();
}

FReply SWidget::OnKeyDown( const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent )
{
	if (bCanSupportFocus && SupportsKeyboardFocus())
	{
		EUINavigation Direction = FSlateApplicationBase::Get().GetNavigationDirectionFromKey(InKeyEvent);
		// It's the left stick return a navigation request of the correct direction
		if (Direction != EUINavigation::Invalid)
		{
			const ENavigationGenesis Genesis = InKeyEvent.GetKey().IsGamepadKey() ? ENavigationGenesis::Controller : ENavigationGenesis::Keyboard;
			return FReply::Handled().SetNavigation(Direction, Genesis);
		}
	}
	return FReply::Unhandled();
}

FReply SWidget::OnKeyUp( const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent )
{
	return FReply::Unhandled();
}

FReply SWidget::OnAnalogValueChanged( const FGeometry& MyGeometry, const FAnalogInputEvent& InAnalogInputEvent )
{
	if (bCanSupportFocus && SupportsKeyboardFocus())
	{
		EUINavigation Direction = FSlateApplicationBase::Get().GetNavigationDirectionFromAnalog(InAnalogInputEvent);
		// It's the left stick return a navigation request of the correct direction
		if (Direction != EUINavigation::Invalid)
		{
			return FReply::Handled().SetNavigation(Direction, ENavigationGenesis::Controller);
		}
	}
	return FReply::Unhandled();
}

FReply SWidget::OnPreviewMouseButtonDown( const FGeometry& MyGeometry, const FPointerEvent& MouseEvent )
{
	return FReply::Unhandled();
}

FReply SWidget::OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (TSharedPtr<FSlateMouseEventsMetaData> Data = GetMetaData<FSlateMouseEventsMetaData>())
	{
		if (Data->MouseButtonDownHandle.IsBound() )
		{
			return Data->MouseButtonDownHandle.Execute(MyGeometry, MouseEvent);
		}
	}
	return FReply::Unhandled();
}

FReply SWidget::OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (TSharedPtr<FSlateMouseEventsMetaData> Data = GetMetaData<FSlateMouseEventsMetaData>())
	{
		if (Data->MouseButtonUpHandle.IsBound())
		{
			return Data->MouseButtonUpHandle.Execute(MyGeometry, MouseEvent);
		}
	}
	return FReply::Unhandled();
}

FReply SWidget::OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (TSharedPtr<FSlateMouseEventsMetaData> Data = GetMetaData<FSlateMouseEventsMetaData>())
	{
		if (Data->MouseMoveHandle.IsBound())
		{
			return Data->MouseMoveHandle.Execute(MyGeometry, MouseEvent);
		}
	}
	return FReply::Unhandled();
}

FReply SWidget::OnMouseButtonDoubleClick(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (TSharedPtr<FSlateMouseEventsMetaData> Data = GetMetaData<FSlateMouseEventsMetaData>())
	{
		if (Data->MouseDoubleClickHandle.IsBound())
		{
			return Data->MouseDoubleClickHandle.Execute(MyGeometry, MouseEvent);
		}
	}
	return FReply::Unhandled();
}

void SWidget::OnMouseEnter( const FGeometry& MyGeometry, const FPointerEvent& MouseEvent )
{
	bIsHovered = true;

	if (TSharedPtr<FSlateMouseEventsMetaData> Data = GetMetaData<FSlateMouseEventsMetaData>())
	{
		if (Data->MouseEnterHandler.IsBound())
		{
			// A valid handler is assigned; let it handle the event.
			Data->MouseEnterHandler.Execute(MyGeometry, MouseEvent);
		}
	}
}

void SWidget::OnMouseLeave( const FPointerEvent& MouseEvent )
{
	bIsHovered = false;

	if (TSharedPtr<FSlateMouseEventsMetaData> Data = GetMetaData<FSlateMouseEventsMetaData>())
	{
		if (Data->MouseLeaveHandler.IsBound())
		{
			// A valid handler is assigned; let it handle the event.
			Data->MouseLeaveHandler.Execute(MouseEvent);
		}
	}
}

FReply SWidget::OnMouseWheel( const FGeometry& MyGeometry, const FPointerEvent& MouseEvent )
{
	return FReply::Unhandled();
}

FCursorReply SWidget::OnCursorQuery( const FGeometry& MyGeometry, const FPointerEvent& CursorEvent ) const
{
	TOptional<EMouseCursor::Type> TheCursor = GetCursor();
	return ( TheCursor.IsSet() )
		? FCursorReply::Cursor( TheCursor.GetValue() )
		: FCursorReply::Unhandled();
}

TOptional<TSharedRef<SWidget>> SWidget::OnMapCursor(const FCursorReply& CursorReply) const
{
	return TOptional<TSharedRef<SWidget>>();
}

bool SWidget::OnVisualizeTooltip( const TSharedPtr<SWidget>& TooltipContent )
{
	return false;
}

TSharedPtr<FPopupLayer> SWidget::OnVisualizePopup(const TSharedRef<SWidget>& PopupContent)
{
	return TSharedPtr<FPopupLayer>();
}

FReply SWidget::OnDragDetected( const FGeometry& MyGeometry, const FPointerEvent& MouseEvent )
{
	return FReply::Unhandled();
}

void SWidget::OnDragEnter( const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent )
{
}

void SWidget::OnDragLeave( const FDragDropEvent& DragDropEvent )
{
}

FReply SWidget::OnDragOver( const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent )
{
	return FReply::Unhandled();
}

FReply SWidget::OnDrop( const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent )
{
	return FReply::Unhandled();
}

FReply SWidget::OnTouchGesture( const FGeometry& MyGeometry, const FPointerEvent& GestureEvent )
{
	return FReply::Unhandled();
}

FReply SWidget::OnTouchStarted( const FGeometry& MyGeometry, const FPointerEvent& InTouchEvent )
{
	return FReply::Unhandled();
}

FReply SWidget::OnTouchMoved( const FGeometry& MyGeometry, const FPointerEvent& InTouchEvent )
{
	return FReply::Unhandled();
}

FReply SWidget::OnTouchEnded( const FGeometry& MyGeometry, const FPointerEvent& InTouchEvent )
{
	return FReply::Unhandled();
}

FReply SWidget::OnTouchForceChanged(const FGeometry& MyGeometry, const FPointerEvent& InTouchEvent)
{
	return FReply::Unhandled();
}

FReply SWidget::OnTouchFirstMove(const FGeometry& MyGeometry, const FPointerEvent& InTouchEvent)
{
	return FReply::Unhandled();
}

FReply SWidget::OnMotionDetected( const FGeometry& MyGeometry, const FMotionEvent& InMotionEvent )
{
	return FReply::Unhandled();
}

TOptional<bool> SWidget::OnQueryShowFocus(const EFocusCause InFocusCause) const
{
	return TOptional<bool>();
}

FPopupMethodReply SWidget::OnQueryPopupMethod() const
{
	return FPopupMethodReply::Unhandled();
}

TSharedPtr<struct FVirtualPointerPosition> SWidget::TranslateMouseCoordinateForCustomHitTestChild(const TSharedRef<SWidget>& ChildWidget, const FGeometry& MyGeometry, const FVector2D& ScreenSpaceMouseCoordinate, const FVector2D& LastScreenSpaceMouseCoordinate) const
{
	return nullptr;
}

void SWidget::OnFinishedPointerInput()
{

}

void SWidget::OnFinishedKeyInput()
{

}

FNavigationReply SWidget::OnNavigation(const FGeometry& MyGeometry, const FNavigationEvent& InNavigationEvent)
{
	EUINavigation Type = InNavigationEvent.GetNavigationType();
	TSharedPtr<FNavigationMetaData> NavigationMetaData = GetMetaData<FNavigationMetaData>();
	if (NavigationMetaData.IsValid())
	{
		TSharedPtr<SWidget> Widget = NavigationMetaData->GetFocusRecipient(Type).Pin();
		return FNavigationReply(NavigationMetaData->GetBoundaryRule(Type), Widget, NavigationMetaData->GetFocusDelegate(Type));
	}
	return FNavigationReply::Escape();
}

EWindowZone::Type SWidget::GetWindowZoneOverride() const
{
	// No special behavior.  Override this in derived widgets, if needed.
	return EWindowZone::Unspecified;
}

void SWidget::Tick( const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime )
{
}

void SWidget::SlatePrepass()
{
	SlatePrepass(FSlateApplicationBase::Get().GetApplicationScale());
}

void SWidget::SlatePrepass(float InLayoutScaleMultiplier)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(SWidget::SlatePrepass);
	SCOPE_CYCLE_COUNTER(STAT_SlatePrepass);

	if (!GSlateIsOnFastUpdatePath || bNeedsPrepass)
	{
		Prepass_Internal(InLayoutScaleMultiplier);
	}
}

void SWidget::InvalidatePrepass()
{
	SLATE_CROSS_THREAD_CHECK();

	TRACE_CPUPROFILER_EVENT_SCOPE(SWidget::InvalidatePrepass);

	bNeedsPrepass = true;
}

void SWidget::InvalidateChildRemovedFromTree(SWidget& Child)
{
	// If the root is invalidated, we need to clear out its PersistentState regardless.
	if (FSlateInvalidationRoot* ChildInvalidationRoot = Child.FastPathProxyHandle.GetInvalidationRootHandle().GetInvalidationRoot())
	{
		SCOPED_NAMED_EVENT(SWidget_InvalidateChildRemovedFromTree, FColor::Red);
		Child.UpdateFastPathVisibility(false, true, ChildInvalidationRoot->GetHittestGrid());
	}
}

FVector2D SWidget::GetDesiredSize() const
{
	return DesiredSize.Get(FVector2D::ZeroVector);
}

void SWidget::AssignParentWidget(TSharedPtr<SWidget> InParent)
{
#if !UE_BUILD_SHIPPING
	ensureMsgf(InParent != SNullWidget::NullWidget, TEXT("The Null Widget can't be anyone's parent."));
	ensureMsgf(this != &SNullWidget::NullWidget.Get(), TEXT("The Null Widget can't have a parent, because a single instance is shared everywhere."));
	ensureMsgf(InParent.IsValid(), TEXT("Are you trying to detatch the parent of a widget?  Use ConditionallyDetatchParentWidget()."));
#endif

	//@todo We should update inherited visibility and volatility here but currently we are relying on ChildOrder invalidation to do that for us

	ParentWidgetPtr = InParent;
#if WITH_ACCESSIBILITY
	if (FSlateApplicationBase::IsInitialized())
	{
		FSlateApplicationBase::Get().GetAccessibleMessageHandler()->MarkDirty();
	}
#endif
	if (InParent.IsValid())
	{
		InParent->Invalidate(EInvalidateWidgetReason::ChildOrder);
	}
}

bool SWidget::ConditionallyDetatchParentWidget(SWidget* InExpectedParent)
{
#if !UE_BUILD_SHIPPING
	ensureMsgf(this != &SNullWidget::NullWidget.Get(), TEXT("The Null Widget can't have a parent, because a single instance is shared everywhere."));
#endif

	TSharedPtr<SWidget> Parent = ParentWidgetPtr.Pin();
	if (Parent.Get() == InExpectedParent)
	{
		ParentWidgetPtr.Reset();
#if WITH_ACCESSIBILITY
		if (FSlateApplicationBase::IsInitialized())
		{
			FSlateApplicationBase::Get().GetAccessibleMessageHandler()->MarkDirty();
		}
#endif

		if (Parent.IsValid())
		{
			Parent->Invalidate(EInvalidateWidgetReason::ChildOrder);
		}

		InvalidateChildRemovedFromTree(*this);
		return true;
	}

	return false;
}

void SWidget::SetFastPathSortOrder(const FSlateInvalidationWidgetSortOrder InSortOrder)
{
	if (InSortOrder != FastPathProxyHandle.GetWidgetSortOrder())
	{
		FastPathProxyHandle.WidgetSortOrder = InSortOrder;
		if (FSlateInvalidationRoot* Root = FastPathProxyHandle.GetInvalidationRootHandle().GetInvalidationRoot())
		{
			if (FHittestGrid* HittestGrid = Root->GetHittestGrid())
			{
				HittestGrid->UpdateWidget(AsShared(), InSortOrder);
			}
		}

		//TODO, update Cached LayerId
	}
}

void SWidget::SetFastPathProxyHandle(const FWidgetProxyHandle& Handle, bool bInInvisibleDueToParentOrSelfVisibility, bool bParentVolatile)
{
	check(this != &SNullWidget::NullWidget.Get());

	FastPathProxyHandle = Handle;

	bInvisibleDueToParentOrSelfVisibility = bInInvisibleDueToParentOrSelfVisibility;
	bInheritedVolatility = bParentVolatile;

	if (bInvisibleDueToParentOrSelfVisibility && PersistentState.CachedElementHandle.IsValid())
	{
#if WITH_SLATE_DEBUGGING
		check(PersistentState.CachedElementHandle.IsOwnedByWidget(this));
#endif
		PersistentState.CachedElementHandle.RemoveFromCache();
	}

	if (IsVolatile() && !IsVolatileIndirectly())
	{
		if (!HasAnyUpdateFlags(EWidgetUpdateFlags::NeedsVolatilePaint))
		{
			AddUpdateFlags(EWidgetUpdateFlags::NeedsVolatilePaint);
		}
	}
	else
	{
		if (HasAnyUpdateFlags(EWidgetUpdateFlags::NeedsVolatilePaint))
		{
			RemoveUpdateFlags(EWidgetUpdateFlags::NeedsVolatilePaint);
		}
	}
}

void SWidget::UpdateFastPathVisibility(bool bParentVisible, bool bWidgetRemoved, FHittestGrid* ParentHittestGrid)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(SWidget::UpdateFastPathVisibility);

	// 如果只是移除Widget，不考虑可见性
	bool bParentAndSelfVisible = bParentVisible;
	bool bFastPathValid = FastPathProxyHandle.IsValid(this);

	FHittestGrid* HittestGridToRemoveFrom = ParentHittestGrid;
	if (!bWidgetRemoved)
	{
		// 获取当前的可见性
		const EVisibility CurrentVisibility = GetVisibility();
		// 父级和自身是否可见
		bParentAndSelfVisible = bParentVisible && CurrentVisibility.IsVisible();
		// 是否由于父级或自身的可见性而不可见
		bInvisibleDueToParentOrSelfVisibility = !bParentAndSelfVisible;

		if (bFastPathValid)
		{
			// 尝试从当前句柄的hit test grid中移除此Widget。如果我们处于嵌套无效化的情况，hit test grid可能已经改变
			HittestGridToRemoveFrom = FastPathProxyHandle.GetInvalidationRoot()->GetHittestGrid();

			// 获取FastPathProxyHandle指向的FWidgetProxy对象
			FWidgetProxy& Proxy = FastPathProxyHandle.GetProxy();

			// 更新FWidgetProxy的Visibility属性
			Proxy.Visibility = CurrentVisibility;
		}
	}
	else if (!bFastPathValid)
	{
		// Widget可能在下一次FastWidgetPathList构建之前被删除。现在从其InvalidationRoot中移除它
		if (FSlateInvalidationRoot* InvalidationRoot = FastPathProxyHandle.GetInvalidationRootHandle().GetInvalidationRoot())
		{
			InvalidationRoot->OnWidgetDestroyed(this);
		}

		FastPathProxyHandle = FWidgetProxyHandle();
	}

	if (HittestGridToRemoveFrom)
	{
		// 从HittestGridToRemoveFrom中移除此Widget
		HittestGridToRemoveFrom->RemoveWidget(SharedThis(this));
	}

	if (bWidgetRemoved)
	{
		// 从缓存中移除PersistentState.CachedElementHandle
		PersistentState.CachedElementHandle.RemoveFromCache();
	}
	else
	{
		// 清除PersistentState.CachedElementHandle的缓存元素
		PersistentState.CachedElementHandle.ClearCachedElements();
	}

	// 遍历所有子Widget
	FChildren* MyChildren = GetAllChildren();
	const int32 NumChildren = MyChildren->Num();
	for (int32 ChildIndex = 0; ChildIndex < NumChildren; ++ChildIndex)
	{
		// 获取子Widget
		const TSharedRef<SWidget>& Child = MyChildren->GetChildAt(ChildIndex);
		// 更新子Widget的快速路径可见性
		Child->UpdateFastPathVisibility(bParentAndSelfVisible, bWidgetRemoved, HittestGridToRemoveFrom);
	}
}

void SWidget::UpdateFastPathVolatility(bool bParentVolatile)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(SWidget::UpdateFastPathVolatility);

	bInheritedVolatility = bParentVolatile;

	if (IsVolatile() && !IsVolatileIndirectly())
	{
		AddUpdateFlags(EWidgetUpdateFlags::NeedsVolatilePaint);
	}
	else
	{
		RemoveUpdateFlags(EWidgetUpdateFlags::NeedsVolatilePaint);
	}

	FChildren* MyChildren = GetAllChildren();
	const int32 NumChildren = MyChildren->Num();
	for (int32 ChildIndex = 0; ChildIndex < NumChildren; ++ChildIndex)
	{
		const TSharedRef<SWidget>& Child = MyChildren->GetChildAt(ChildIndex);
		Child->UpdateFastPathVolatility(bParentVolatile || IsVolatile());
	}


}

void SWidget::CacheDesiredSize(float InLayoutScaleMultiplier)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(SWidget::CacheDesiredSize);

#if SLATE_VERBOSE_NAMED_EVENTS
	SCOPED_NAMED_EVENT(SWidget_CacheDesiredSize, FColor::Red);
#endif

	// Cache this widget's desired size.
	SetDesiredSize(ComputeDesiredSize(InLayoutScaleMultiplier));
}


bool SWidget::SupportsKeyboardFocus() const
{
	return false;
}

bool SWidget::HasKeyboardFocus() const
{
	return (FSlateApplicationBase::Get().GetKeyboardFocusedWidget().Get() == this);
}

TOptional<EFocusCause> SWidget::HasUserFocus(int32 UserIndex) const
{
	return FSlateApplicationBase::Get().HasUserFocus(SharedThis(this), UserIndex);
}

TOptional<EFocusCause> SWidget::HasAnyUserFocus() const
{
	return FSlateApplicationBase::Get().HasAnyUserFocus(SharedThis(this));
}

bool SWidget::HasUserFocusedDescendants(int32 UserIndex) const
{
	return FSlateApplicationBase::Get().HasUserFocusedDescendants(SharedThis(this), UserIndex);
}

bool SWidget::HasFocusedDescendants() const
{
	return FSlateApplicationBase::Get().HasFocusedDescendants(SharedThis(this));
}

bool SWidget::HasAnyUserFocusOrFocusedDescendants() const
{
	return HasAnyUserFocus().IsSet() || HasFocusedDescendants();
}

const FSlateBrush* SWidget::GetFocusBrush() const
{
	return FCoreStyle::Get().GetBrush("FocusRectangle");
}

bool SWidget::HasMouseCapture() const
{
	return FSlateApplicationBase::Get().DoesWidgetHaveMouseCapture(SharedThis(this));
}

bool SWidget::HasMouseCaptureByUser(int32 UserIndex, TOptional<int32> PointerIndex) const
{
	return FSlateApplicationBase::Get().DoesWidgetHaveMouseCaptureByUser(SharedThis(this), UserIndex, PointerIndex);
}

void SWidget::OnMouseCaptureLost(const FCaptureLostEvent& CaptureLostEvent)
{
}

bool SWidget::FindChildGeometries( const FGeometry& MyGeometry, const TSet< TSharedRef<SWidget> >& WidgetsToFind, TMap<TSharedRef<SWidget>, FArrangedWidget>& OutResult ) const
{
	FindChildGeometries_Helper(MyGeometry, WidgetsToFind, OutResult);
	return OutResult.Num() == WidgetsToFind.Num();
}


void SWidget::FindChildGeometries_Helper( const FGeometry& MyGeometry, const TSet< TSharedRef<SWidget> >& WidgetsToFind, TMap<TSharedRef<SWidget>, FArrangedWidget>& OutResult ) const
{
	// Perform a breadth first search!

	FArrangedChildren ArrangedChildren(EVisibility::Visible);
	this->ArrangeChildren(MyGeometry, ArrangedChildren);
	const int32 NumChildren = ArrangedChildren.Num();

	// See if we found any of the widgets on this level.
	for(int32 ChildIndex=0; ChildIndex < NumChildren; ++ChildIndex )
	{
		const FArrangedWidget& CurChild = ArrangedChildren[ ChildIndex ];
		
		if ( WidgetsToFind.Contains(CurChild.Widget) )
		{
			// We found one of the widgets for which we need geometry!
			OutResult.Add( CurChild.Widget, CurChild );
		}
	}

	// If we have not found all the widgets that we were looking for, descend.
	if ( OutResult.Num() != WidgetsToFind.Num() )
	{
		// Look for widgets among the children.
		for( int32 ChildIndex=0; ChildIndex < NumChildren; ++ChildIndex )
		{
			const FArrangedWidget& CurChild = ArrangedChildren[ ChildIndex ];
			CurChild.Widget->FindChildGeometries_Helper( CurChild.Geometry, WidgetsToFind, OutResult );
		}	
	}	
}

FGeometry SWidget::FindChildGeometry( const FGeometry& MyGeometry, TSharedRef<SWidget> WidgetToFind ) const
{
	// We just need to find the one WidgetToFind among our descendants.
	TSet< TSharedRef<SWidget> > WidgetsToFind;
	{
		WidgetsToFind.Add( WidgetToFind );
	}
	TMap<TSharedRef<SWidget>, FArrangedWidget> Result;

	FindChildGeometries( MyGeometry, WidgetsToFind, Result );

	return Result.FindChecked( WidgetToFind ).Geometry;
}

int32 SWidget::FindChildUnderMouse( const FArrangedChildren& Children, const FPointerEvent& MouseEvent )
{
	const FVector2D& AbsoluteCursorLocation = MouseEvent.GetScreenSpacePosition();
	return SWidget::FindChildUnderPosition( Children, AbsoluteCursorLocation );
}

int32 SWidget::FindChildUnderPosition( const FArrangedChildren& Children, const FVector2D& ArrangedSpacePosition )
{
	const int32 NumChildren = Children.Num();
	for( int32 ChildIndex=NumChildren-1; ChildIndex >= 0; --ChildIndex )
	{
		const FArrangedWidget& Candidate = Children[ChildIndex];
		const bool bCandidateUnderCursor = 
			// Candidate is physically under the cursor
			Candidate.Geometry.IsUnderLocation( ArrangedSpacePosition );

		if (bCandidateUnderCursor)
		{
			return ChildIndex;
		}
	}

	return INDEX_NONE;
}

FString SWidget::ToString() const
{
	return FString::Printf(TEXT("%s [%s]"), *this->TypeOfWidget.ToString(), *this->GetReadableLocation() );
}

FString SWidget::GetTypeAsString() const
{
	return this->TypeOfWidget.ToString();
}

FName SWidget::GetType() const
{
	return TypeOfWidget;
}

FString SWidget::GetReadableLocation() const
{
#if !UE_BUILD_SHIPPING
	return FString::Printf(TEXT("%s(%d)"), *FPaths::GetCleanFilename(this->CreatedInLocation.GetPlainNameString()), this->CreatedInLocation.GetNumber());
#else
	return FString();
#endif
}

FName SWidget::GetCreatedInLocation() const
{
#if !UE_BUILD_SHIPPING
	return CreatedInLocation;
#else
	return NAME_None;
#endif
}

FName SWidget::GetTag() const
{
	return Tag;
}

FSlateColor SWidget::GetForegroundColor() const
{
	static FSlateColor NoColor = FSlateColor::UseForeground();
	return NoColor;
}

const FGeometry& SWidget::GetCachedGeometry() const
{
	return GetTickSpaceGeometry();
}

const FGeometry& SWidget::GetTickSpaceGeometry() const
{
	return PersistentState.DesktopGeometry;
}

const FGeometry& SWidget::GetPaintSpaceGeometry() const
{
	return PersistentState.AllottedGeometry;
}

namespace Private
{
	TSharedPtr<FSlateToolTipMetaData> FindOrAddToolTipMetaData(SWidget* Widget)
	{
		TSharedPtr<FSlateToolTipMetaData> Data = Widget->GetMetaData<FSlateToolTipMetaData>();
		if (!Data)
		{
			Data = MakeShared<FSlateToolTipMetaData>();
			Widget->AddMetadata(Data.ToSharedRef());
		}
		return Data;
	}
}

void SWidget::SetToolTipText(const TAttribute<FText>& ToolTipText)
{
	if (ToolTipText.IsSet())
	{
		Private::FindOrAddToolTipMetaData(this)->ToolTip = FSlateApplicationBase::Get().MakeToolTip(ToolTipText);
	}
	else
	{
		RemoveMetaData<FSlateToolTipMetaData>();
	}
}

void SWidget::SetToolTipText( const FText& ToolTipText )
{
	if (!ToolTipText.IsEmptyOrWhitespace())
	{
		Private::FindOrAddToolTipMetaData(this)->ToolTip = FSlateApplicationBase::Get().MakeToolTip(ToolTipText);
	}
	else
	{
		RemoveMetaData<FSlateToolTipMetaData>();
	}
}

void SWidget::SetToolTip(const TAttribute<TSharedPtr<IToolTip>>& InToolTip)
{
	if (InToolTip.IsSet())
	{
		Private::FindOrAddToolTipMetaData(this)->ToolTip = InToolTip;
	}
	else
	{
		RemoveMetaData<FSlateToolTipMetaData>();
	}
}

TSharedPtr<IToolTip> SWidget::GetToolTip()
{
	if (TSharedPtr<FSlateToolTipMetaData> Data = GetMetaData<FSlateToolTipMetaData>())
	{
		return Data->ToolTip.Get();
	}
	return TSharedPtr<IToolTip>();
}

void SWidget::OnToolTipClosing()
{
}

void SWidget::EnableToolTipForceField( const bool bEnableForceField )
{
	bToolTipForceFieldEnabled = bEnableForceField;
}

bool SWidget::IsDirectlyHovered() const
{
	return FSlateApplicationBase::Get().IsWidgetDirectlyHovered(SharedThis(this));
}

void SWidget::SetVisibility(TAttribute<EVisibility> InVisibility)
{
	SetAttribute(Visibility, InVisibility, EInvalidateWidgetReason::Visibility);
}

void SWidget::Invalidate(EInvalidateWidgetReason InvalidateReason)
{
	SLATE_CROSS_THREAD_CHECK();
	
#if UE_TRACE_ENABLED
	static FString ReasonStrArr[10]={
		"SWidget::Invalidate(EInvalidateWidgetReason::None)(",
		"SWidget::Invalidate(EInvalidateWidgetReason::Layout)(",
		"SWidget::Invalidate(EInvalidateWidgetReason::Paint)(",
		"SWidget::Invalidate(EInvalidateWidgetReason::Volatility)(",
		"SWidget::Invalidate(EInvalidateWidgetReason::ChildOrder)(",
		"SWidget::Invalidate(EInvalidateWidgetReason::RenderTransform)(",
		"SWidget::Invalidate(EInvalidateWidgetReason::Visibility)(",
		"SWidget::Invalidate(EInvalidateWidgetReason::PaintAndVolatility)(",
		"SWidget::Invalidate(EInvalidateWidgetReason::LayoutAndVolatility)(",
		"SWidget::Invalidate(EInvalidateWidgetReason::All)(",
	};
	FString EventName;
	switch (InvalidateReason)
	{
	case EInvalidateWidgetReason::None: EventName=ReasonStrArr[0];break;
	case EInvalidateWidgetReason::Layout: EventName=ReasonStrArr[1];break;
	case EInvalidateWidgetReason::Paint: EventName=ReasonStrArr[2];break;
	case EInvalidateWidgetReason::Volatility: EventName=ReasonStrArr[3];break;
	case EInvalidateWidgetReason::ChildOrder: EventName=ReasonStrArr[4];break;
	case EInvalidateWidgetReason::RenderTransform: EventName=ReasonStrArr[5];break;
	case EInvalidateWidgetReason::Visibility: EventName=ReasonStrArr[6];break;
	case EInvalidateWidgetReason::PaintAndVolatility: EventName=ReasonStrArr[7];break;
	case EInvalidateWidgetReason::LayoutAndVolatility: EventName=ReasonStrArr[8];break;
	case EInvalidateWidgetReason::All: EventName=ReasonStrArr[9];break;
	default: ;
	}
	EventName=EventName+Tag.ToString()+TEXT(")");
	TRACE_CPUPROFILER_EVENT_SCOPE_TEXT_ON_CHANNEL(*EventName, CpuChannel);
#endif

	// SCOPED_NAMED_EVENT_TEXT("SWidget::Invalidate", FColor::Orange);
	const bool bWasVolatile = IsVolatileIndirectly() || IsVolatile();

	// Backwards compatibility fix:  Its no longer valid to just invalidate volatility since we need to repaint to cache elements if a widget becomes non-volatile. So after volatility changes force repaint
	if (InvalidateReason == EInvalidateWidgetReason::Volatility)
	{
		InvalidateReason = EInvalidateWidgetReason::PaintAndVolatility;
	}

	const bool bVolatilityChanged = EnumHasAnyFlags(InvalidateReason, EInvalidateWidgetReason::Volatility) ? Advanced_InvalidateVolatility() : false;

	if (EnumHasAnyFlags(InvalidateReason, EInvalidateWidgetReason::ChildOrder) || !PrepassLayoutScaleMultiplier.IsSet())
	{
		InvalidatePrepass();
	}

	if(FastPathProxyHandle.IsValid(this))
	{
		// 当前的想法是，可见性和易变性应该立即更新，而不是在下一帧的快速路径无效化处理过程中更新。就是说触发Invalidate的Reason是Visibility时，立即更新。例如在逻辑代码中设置Collapsed，就需要立即更新。
		if (EnumHasAnyFlags(InvalidateReason, EInvalidateWidgetReason::Visibility))
		{
			TSharedPtr<SWidget> ParentWidget = GetParentWidget();
			UpdateFastPathVisibility(ParentWidget.IsValid() ? !ParentWidget->bInvisibleDueToParentOrSelfVisibility : false, false, FastPathProxyHandle.GetInvalidationRoot()->GetHittestGrid());
		}

		if (bVolatilityChanged)
		{
			SCOPED_NAMED_EVENT(SWidget_UpdateFastPathVolatility, FColor::Red);

			TSharedPtr<SWidget> ParentWidget = GetParentWidget();

			UpdateFastPathVolatility(ParentWidget.IsValid() ? ParentWidget->IsVolatile() || ParentWidget->IsVolatileIndirectly() : false);

			ensure(!IsVolatile() || IsVolatileIndirectly() || EnumHasAnyFlags(UpdateFlags, EWidgetUpdateFlags::NeedsVolatilePaint));
		}

		FastPathProxyHandle.MarkWidgetDirty(InvalidateReason);
	}
	else
	{
#if WITH_SLATE_DEBUGGING
		FSlateDebugging::BroadcastWidgetInvalidate(this, nullptr, InvalidateReason);
#endif
		UE_TRACE_SLATE_WIDGET_INVALIDATED(this, nullptr, InvalidateReason);
	}
}

void SWidget::SetCursor( const TAttribute< TOptional<EMouseCursor::Type> >& InCursor )
{
	// If bounded or has a valid optional value
	if (InCursor.IsBound() || InCursor.Get().IsSet())
	{
		TSharedPtr<FSlateCursorMetaData> Data = GetMetaData<FSlateCursorMetaData>();
		if (!Data)
		{
			Data = MakeShared<FSlateCursorMetaData>();
			AddMetadata(Data.ToSharedRef());
		}
		Data->Cursor = InCursor;
	}
	else
	{
		RemoveMetaData<FSlateCursorMetaData>();
	}
}

TOptional<EMouseCursor::Type> SWidget::GetCursor() const
{
	if (TSharedPtr<FSlateCursorMetaData> Data = GetMetaData<FSlateCursorMetaData>())
	{
		return Data->Cursor.Get();
	}
	return TOptional<EMouseCursor::Type>();
}

void SWidget::SetDebugInfo( const ANSICHAR* InType, const ANSICHAR* InFile, int32 OnLine, size_t InAllocSize )
{
	TypeOfWidget = InType;

	STAT(AllocSize = InAllocSize);
	INC_MEMORY_STAT_BY(STAT_SlateSWidgetAllocSize, AllocSize);

#if !UE_BUILD_SHIPPING
	CreatedInLocation = FName( InFile );
	CreatedInLocation.SetNumber(OnLine);
#endif

	UE_TRACE_SLATE_WIDGET_DEBUG_INFO(this);
}

void SWidget::OnClippingChanged()
{

}

FSlateRect SWidget::CalculateCullingAndClippingRules(const FGeometry& AllottedGeometry, const FSlateRect& IncomingCullingRect, bool& bClipToBounds, bool& bAlwaysClip, bool& bIntersectClipBounds) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(SWidget::CalculateCullingAndClippingRules);
	
	bClipToBounds = false;
	bIntersectClipBounds = true;
	bAlwaysClip = false;

	if (!bClippingProxy)
	{
		switch (Clipping)
		{
		case EWidgetClipping::ClipToBounds:
			bClipToBounds = true;
			break;
		case EWidgetClipping::ClipToBoundsAlways:
			bClipToBounds = true;
			bAlwaysClip = true;
			break;
		case EWidgetClipping::ClipToBoundsWithoutIntersecting:
			bClipToBounds = true;
			bIntersectClipBounds = false;
			break;
		case EWidgetClipping::OnDemand:
			const float OverflowEpsilon = 1.0f;
			const FVector2D& CurrentSize = GetDesiredSize();
			const FVector2D& LocalSize = AllottedGeometry.GetLocalSize();
			bClipToBounds =
				(CurrentSize.X - OverflowEpsilon) > LocalSize.X ||
				(CurrentSize.Y - OverflowEpsilon) > LocalSize.Y;
			break;
		}
	}

	if (bClipToBounds)
	{
		FSlateRect MyCullingRect(AllottedGeometry.GetRenderBoundingRect(CullingBoundsExtension));

		if (bIntersectClipBounds)
		{
			bool bClipBoundsOverlapping;
			return IncomingCullingRect.IntersectionWith(MyCullingRect, bClipBoundsOverlapping);
		}
		
		return MyCullingRect;
	}

	return IncomingCullingRect;
}

/**
 * @brief 绘制一个窗口部件。
 * 
 * @param Args 绘制参数，包含了绘制所需的一些参数。
 * @param AllottedGeometry 部件的几何形状。
 * @param MyCullingRect 部件的裁剪矩形。
 * @param OutDrawElements 用于存储绘制元素的列表。
 * @param LayerId 起始的图层ID。
 * @param InWidgetStyle 部件的样式。
 * @param bParentEnabled 父部件是否启用。
 * @return int32 返回绘制过程中的最大图层ID。
 */
int32 SWidget::Paint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
#if UE_TRACE_ENABLED
	if(TraceStringForPaint.IsEmpty())
	{
		TraceStringForPaint = TEXT("SWidget::Paint(")+Tag.ToString()+TEXT(")");
	}
	TRACE_CPUPROFILER_EVENT_SCOPE_TEXT_ON_CHANNEL(*TraceStringForPaint, CpuChannel);
#endif
	
	// 获取当前的更新标志（PreviousUpdateFlag）
	const EWidgetUpdateFlags PreviousUpdateFlag = UpdateFlags;

	
	// 📌  unfiled/wn5cL1dWTjuRiysZ1YLabF.md 📝 🗑️
	TSharedRef<SWidget> MutableThis = ConstCastSharedRef<SWidget>(AsShared());

	// 增加一个统计项，记录绘制的widget数量（STAT_SlateNumPaintedWidgets）。
	INC_DWORD_STAT(STAT_SlateNumPaintedWidgets);

	// 记录这个widget的绘制事件，用于性能分析。
	UE_TRACE_SCOPED_SLATE_WIDGET_PAINT(this);

	// 获取当前widget的父widget（PaintParent）。
	const SWidget* PaintParent = Args.GetPaintParent();
	//if (GSlateEnableGlobalInvalidation)
	//{
	//	bInheritedVolatility = PaintParent ? (PaintParent->IsVolatileIndirectly() || PaintParent->IsVolatile()) : false;
	//}


	// If this widget clips to its bounds, then generate a new clipping rect representing the intersection of the bounding
	// rectangle of the widget's geometry, and the current clipping rectangle.
	bool bClipToBounds, bAlwaysClip, bIntersectClipBounds;

	// 根据给定的几何信息和裁剪矩形，计算出裁剪和剪裁的规则和边界。
	FSlateRect CullingBounds = CalculateCullingAndClippingRules(AllottedGeometry, MyCullingRect, bClipToBounds, bAlwaysClip, bIntersectClipBounds);

	// 创建一个FWidgetStyle对象，用于设置widget的样式。这里主要是设置了widget的透明度。
	FWidgetStyle ContentWidgetStyle = FWidgetStyle(InWidgetStyle)
		.BlendOpacity(RenderOpacity);

	
	// 缓存tick的几何信息，以便外部用户获取上次使用的几何信息，或者本应用于tick Widget的几何信息。
	// 缓存几何信息，以便在tick函数中使用。这里首先复制了AllottedGeometry，然后将窗口到桌面的变换应用到几何信息上。
	// 📌  unfiled/dS8EE8RCVLidK5vcc67TGF.md 📝 🗑️
	FGeometry DesktopSpaceGeometry = AllottedGeometry;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(DesktopSpaceGeometry_AppendTransform);
		DesktopSpaceGeometry.AppendTransform(FSlateLayoutTransform(Args.GetWindowToDesktopTransform()));
	}

	// 如果widget有活动的计时器需要更新，那么执行ExecuteActiveTimers函数。
	if (HasAnyUpdateFlags(EWidgetUpdateFlags::NeedsActiveTimerUpdate))
	{
		SCOPE_CYCLE_COUNTER(STAT_SlateExecuteActiveTimers);
		TRACE_CPUPROFILER_EVENT_SCOPE(SWidget::ExecuteActiveTimers);

		// 执行widget的所有Active Timers。
		// Args.GetCurrentTime()获取当前的时间，Args.GetDeltaTime()获取上一帧到当前帧的时间差，这两个参数通常用于计算动画或其他需要时间信息的操作。
		MutableThis->ExecuteActiveTimers(Args.GetCurrentTime(), Args.GetDeltaTime());
	}

	// 如果widget需要执行tick函数，那么执行Tick函数。
	if (HasAnyUpdateFlags(EWidgetUpdateFlags::NeedsTick))
	{
		INC_DWORD_STAT(STAT_SlateNumTickedWidgets);

		SCOPE_CYCLE_COUNTER(STAT_SlateTickWidgets);
		SCOPE_CYCLE_SWIDGET(this);

		TRACE_CPUPROFILER_EVENT_SCOPE(SWidget::Tick);
		MutableThis->Tick(DesktopSpaceGeometry, Args.GetCurrentTime(), Args.GetDeltaTime());
	}

	// 这两行代码处理了命中测试的可见性。命中测试是用于确定用户点击或触摸的是哪个widget。

	// bInheritedHittestability获取父widget的命中测试可见性
	const bool bInheritedHittestability = Args.GetInheritedHittestability();

	// bOutgoingHittestability则根据父widget和当前widget的可见性确定当前widget的命中测试可见性。
	// 例如，如果父widget不可点击，那么子widget也不应该被点击，即使它自己是可见的。
	const bool bOutgoingHittestability = bInheritedHittestability && GetVisibility().AreChildrenHitTestVisible();

#if WITH_SLATE_DEBUGGING
	if (GDebugCulling)
	{
		// When we're debugging culling, don't actually clip, we'll just pretend to, so we can see the effects of
		// any widget doing culling to know if it's doing the right thing.
		bClipToBounds = false;
	}

	// 📌  unfiled/ke5LJ9r7KLVinUZs31oisv.md 📝 🗑️
#endif

	// 获取父级widget，如果存在则将其设置为当前widget的PaintParent
	SWidget* PaintParentPtr = const_cast<SWidget*>(Args.GetPaintParent());
	ensure(PaintParentPtr != this);
	if (PaintParentPtr)
	{
		PersistentState.PaintParent = PaintParentPtr->AsShared();
	}
	else
	{
		PaintParentPtr = nullptr;
	}
	
	// @todo This should not do this copy if the clipping state is unset
	// 设置PersistentState的各个属性，保存当前widget的状态信息，以便在需要时可以恢复这些状态。
	// 📌  unfiled/kyvidNF4mZKRvH6t2Ph8FX.md 📝 🗑️

	PersistentState.InitialClipState = OutDrawElements.GetClippingState(); // 初始剪裁状态
	PersistentState.LayerId = LayerId; // 层级ID
	PersistentState.bParentEnabled = bParentEnabled; // 父级是否启用
	PersistentState.bInheritedHittestability = bInheritedHittestability; // 继承的可点击性
	PersistentState.AllottedGeometry = AllottedGeometry; // 分配的几何形状
	PersistentState.DesktopGeometry = DesktopSpaceGeometry; // 桌面空间几何形状
	PersistentState.WidgetStyle = InWidgetStyle; // widget样式
	PersistentState.CullingBounds = MyCullingRect; // 剔除边界
	PersistentState.IncomingUserIndex = Args.GetHittestGrid().GetUserIndex(); // 用户索引
	PersistentState.IncomingFlowDirection = GSlateFlowDirection; // 流向
																 
	// 📌  unfiled/3gdp5nEJu4tFAFS2ZzzEvU.md 📝 🗑️

	// 更新FPaintArgs的参数，以便在绘制子widget时，可以将当前widget作为父widget。
	FPaintArgs UpdatedArgs = Args.WithNewParent(this);

	// 📌  unfiled/1u3HC5Qc1DCZC3izs4Lv2i.md 📝 🗑️
	UpdatedArgs.SetInheritedHittestability(bOutgoingHittestability);

#if 0
	// test ensure that we are not the last thing holding this widget together
	ensure(!MutableThis.IsUnique());

	// 📌  unfiled/7zEamyvzFFLHriojRTVWWu.md 📝 🗑️
#endif

#if WITH_SLATE_DEBUGGING
	if (!FastPathProxyHandle.IsValid(this) && PersistentState.CachedElementHandle.IsValid())
	{
		ensure(!bInvisibleDueToParentOrSelfVisibility);
	}
#endif

	// 将当前widget添加到当前Window的绘制元素列表(FSlateWindowElementList)中，LayerId是当前widget的层级，PersistentState.CachedElementHandle是当前widget的缓存元素句柄。
	OutDrawElements.PushPaintingWidget(*this, LayerId, PersistentState.CachedElementHandle);

	// 如果当前widget是可点击的（bOutgoingHittestability为true），则将其添加到点击测试网格中。点击测试网格用于在用户点击屏幕时确定被点击的widget。
	if (bOutgoingHittestability)
	{
		Args.GetHittestGrid().AddWidget(MutableThis, 0, LayerId, FastPathProxyHandle.GetWidgetSortOrder());
	}

	// 如果当前widget需要被裁剪（bClipToBounds为true），则创建一个裁剪区域并将其添加到绘制元素列表中。
	// 📌  unfiled/9ikLUDCTAW5YAGTid8HgJ8.md 📝 🗑️
	if (bClipToBounds)
	{
		// This sets up the clip state for any children NOT myself
		FSlateClippingZone ClippingZone(AllottedGeometry);
		ClippingZone.SetShouldIntersectParent(bIntersectClipBounds);
		ClippingZone.SetAlwaysClip(bAlwaysClip);
		OutDrawElements.PushClip(ClippingZone);
	}
#if WITH_SLATE_DEBUGGING
	// 广播一个BeginWidgetPaint事件。这个事件表示一个widget开始绘制。
	FSlateDebugging::BeginWidgetPaint.Broadcast(this, UpdatedArgs, AllottedGeometry, CullingBounds, OutDrawElements, LayerId);
#endif

	// Establish the flow direction if we're changing from inherit.
	// FOR RB mode, this should first set GSlateFlowDirection to the incoming state that was cached for the widget, then paint
	// will override it here to reflow is needed.
	// 📌  unfiled/rreWKfJAgdp3ozrZDhPLK5.md 📝 🗑️
	TGuardValue<EFlowDirection> FlowGuard(GSlateFlowDirection, ComputeFlowDirection());

#if WITH_SLATE_DEBUGGING
	
	TArray<TWeakPtr<const SWidget>> DebugChildWidgetsToPaint;

	if (GSlateIsOnFastUpdatePath && GSlateEnsureAllVisibleWidgetsPaint)
	{
		// Don't check things that are invalidation roots, or volatile, or volatile indirectly, a completely different set
		// of rules apply to those widgets.
		if (!IsVolatile() && !IsVolatileIndirectly() && !Advanced_IsInvalidationRoot())
		{
			// 收集所有可见的子widget，以便在绘制阶段进行绘制。
			const FChildren* MyChildren = MutableThis->GetChildren();
			const int32 NumChildren = MyChildren->Num();
			for (int32 ChildIndex = 0; ChildIndex < MyChildren->Num(); ++ChildIndex)
			{
				TSharedRef<const SWidget> Child = MyChildren->GetChildAt(ChildIndex);
				if (Child->GetVisibility().IsVisible())
				{
					DebugChildWidgetsToPaint.Add(Child);
				}
			}
		}
	}

	// 📌  unfiled/tstLcZdAw6pV9EEGuZy4wh.md 📝 🗑️
#endif
	
	// Paint the geometry of this widget.
	int32 NewLayerId = 0;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(OnPaint);

		#if UE_TRACE_ENABLED
		// 记录即将生成FSlateDrawElement的Widget的Tag
		FSlateDrawElement::WidgetTagToBeProcessed=Tag;
		#endif

		// 📌  unfiled/u6NoYf5VgVCuxpqY23gjTe.md 📝 🗑️
		// 调用了 OnPaint 函数来绘制当前的 widget。OnPaint 是一个虚函数，它在每个 widget 类中都可能被重写。
		NewLayerId = OnPaint(UpdatedArgs, AllottedGeometry, CullingBounds, OutDrawElements, LayerId, ContentWidgetStyle, bParentEnabled);
	}

	// Just repainted
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(RemoveUpdateFlags);
		MutableThis->RemoveUpdateFlags(EWidgetUpdateFlags::NeedsRepaint);

		// 📌  unfiled/qKBG4A3rcgdiAEoNvJwDvg.md 📝 🗑️
	}

	// Detect children that should have been painted, but were skipped during the paint process.
	// this will result in geometry being left on screen and not cleared, because it's visible, yet wasn't painted.
#if WITH_SLATE_DEBUGGING
	// 检测那些应该被绘制但在绘制过程中被跳过的子widget。
	// 如果一个widget是可见的但没有被绘制，那么它的几何形状将会被留在屏幕上并且不会被清除。
	if (GSlateIsOnFastUpdatePath && GSlateEnsureAllVisibleWidgetsPaint)
	{
		for (TWeakPtr<const SWidget>& DebugChildThatShouldHaveBeenPaintedPtr : DebugChildWidgetsToPaint)
		{
			if (TSharedPtr<const SWidget> DebugChild = DebugChildThatShouldHaveBeenPaintedPtr.Pin())
			{
				if (DebugChild->GetVisibility().IsVisible())
				{
					ensureMsgf(DebugChild->Debug_GetLastPaintFrame() == GFrameNumber, TEXT("The Widget '%s' was visible, but never painted.  This means it was skipped during painting, without alerting the fast path."), *FReflectionMetaData::GetWidgetPath(DebugChild.Get()));
				}
			}
		}
	}

	// 📌  unfiled/cg5Sht6i6mbtnciUC9uWSx.md 📝 🗑️
#endif

	// Draw the clipping zone if we've got clipping enabled
#if WITH_SLATE_DEBUGGING
	FSlateDebugging::EndWidgetPaint.Broadcast(this, OutDrawElements, NewLayerId);

	if (GShowClipping && bClipToBounds)
	{
		FSlateClippingZone ClippingZone(AllottedGeometry);

		TArray<FVector2D> Points;
		Points.Add(ClippingZone.TopLeft);
		Points.Add(ClippingZone.TopRight);
		Points.Add(ClippingZone.BottomRight);
		Points.Add(ClippingZone.BottomLeft);
		Points.Add(ClippingZone.TopLeft);

		const bool bAntiAlias = true;
		FSlateDrawElement::MakeLines(
			OutDrawElements,
			NewLayerId,
			FPaintGeometry(),
			Points,
			ESlateDrawEffect::None,
			ClippingZone.IsAxisAligned() ? FLinearColor::Yellow : FLinearColor::Red,
			bAntiAlias,
			2.0f);
	}

	// 📌  unfiled/5KobrSxiMRrWTmgu49jmF2.md 📝 🗑️
#endif // WITH_SLATE_DEBUGGING


	if (bClipToBounds)
	{
		OutDrawElements.PopClip(); // 移除当前widget的裁剪区域 // 📌  unfiled/anHm96CsRwBUpEReQuGBaG.md 📝 🗑️
	}

#if PLATFORM_UI_NEEDS_FOCUS_OUTLINES
	// 绘制键盘焦点轮廓。
	// 检查当前的widget是否可以支持焦点
	// Check if we need to show the keyboard focus ring, this is only necessary if the widget could be focused.
	if (bCanSupportFocus && SupportsKeyboardFocus())
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(ShowUserFocus);
		
		// 检查是否需要显示用户焦点
		bool bShowUserFocus = FSlateApplicationBase::Get().ShowUserFocus(SharedThis(this));
		if (bShowUserFocus)
		{
			// 获取焦点轮廓的画刷
			const FSlateBrush* BrushResource = GetFocusBrush();

			if (BrushResource != nullptr)
			{
				// 绘制一个矩形，这个矩形的位置和大小由AllottedGeometry决定，画刷由BrushResource提供。
				FSlateDrawElement::MakeBox(
					OutDrawElements,
					NewLayerId,
					AllottedGeometry.ToPaintGeometry(),
					BrushResource,
					ESlateDrawEffect::None,
					BrushResource->GetTint(InWidgetStyle)
				);
			}
		}
	}
#endif

	// 从OutDrawElements中弹出当前widget的绘制元素缓存句柄。
	FSlateCachedElementsHandle NewCacheHandle = OutDrawElements.PopPaintingWidget(*this);

	// 📌  unfiled/oScW7AMxHaEY8NKVgcsdN9.md 📝 🗑️
	// 如果需要解决延迟绘制
	if (OutDrawElements.ShouldResolveDeferred())
	{
		// 解决延迟绘制。
		NewLayerId = OutDrawElements.PaintDeferred(NewLayerId, MyCullingRect);
	}

	// 📌  unfiled/wBTYPxbZu7rkY7rtzSWLzb.md 📝 🗑️
	// 更新widget的代理。
	MutableThis->UpdateWidgetProxy(NewLayerId, NewCacheHandle);

	// 如果启用了Slate调试，那么就广播一个widget更新事件。
#if WITH_SLATE_DEBUGGING
	FSlateDebugging::BroadcastWidgetUpdatedByPaint(this, PreviousUpdateFlag);
#endif
	UE_TRACE_SLATE_WIDGET_UPDATED(this, PreviousUpdateFlag);

	return NewLayerId;
}

float SWidget::GetRelativeLayoutScale(int32 ChildIndex, float LayoutScaleMultiplier) const
{
	return 1.0f;
}

void SWidget::ArrangeChildren(const FGeometry& AllottedGeometry, FArrangedChildren& ArrangedChildren) const
{
#if WITH_VERY_VERBOSE_SLATE_STATS
	SCOPED_NAMED_EVENT(SWidget_ArrangeChildren, FColor::Black);
#endif
	OnArrangeChildren(AllottedGeometry, ArrangedChildren);
}

void SWidget::Prepass_Internal(float InLayoutScaleMultiplier)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(SWidget::Prepass_Internal);
	
	PrepassLayoutScaleMultiplier = InLayoutScaleMultiplier;

	bool bShouldPrepassChildren = true;
	if (bHasCustomPrepass)
	{
		bShouldPrepassChildren = CustomPrepass(InLayoutScaleMultiplier);
	}

	if (bCanHaveChildren && bShouldPrepassChildren)
	{
		// Cache child desired sizes first. This widget's desired size is
		// a function of its children's sizes.
		FChildren* MyChildren = this->GetChildren();
		const int32 NumChildren = MyChildren->Num();
		Prepass_ChildLoop(InLayoutScaleMultiplier, MyChildren);
		ensure(NumChildren == MyChildren->Num());
	}

	{
		// Cache this widget's desired size.
		CacheDesiredSize(PrepassLayoutScaleMultiplier.Get(1.0f));
		bNeedsPrepass = false;
	}
}

void SWidget::Prepass_ChildLoop(float InLayoutScaleMultiplier, FChildren* MyChildren)
{
	const int32 NumChildren = MyChildren->Num();
	for (int32 ChildIndex = 0; ChildIndex < MyChildren->Num(); ++ChildIndex)
	{
		const float ChildLayoutScaleMultiplier = bHasRelativeLayoutScale
			? InLayoutScaleMultiplier * GetRelativeLayoutScale(ChildIndex, InLayoutScaleMultiplier)
			: InLayoutScaleMultiplier;

		const TSharedRef<SWidget>& Child = MyChildren->GetChildAt(ChildIndex);

		if (Child->Visibility.Get() != EVisibility::Collapsed)
		{
			// Recur: Descend down the widget tree.
			Child->Prepass_Internal(ChildLayoutScaleMultiplier);
		}
		else
		{
			// If the child widget is collapsed, we need to store the new layout scale it will have when 
			// it is finally visible and invalidate it's prepass so that it gets that when its visiblity
			// is finally invalidated.
			Child->InvalidatePrepass();
			Child->PrepassLayoutScaleMultiplier = ChildLayoutScaleMultiplier;
		}
	}
}

TSharedRef<FActiveTimerHandle> SWidget::RegisterActiveTimer(float TickPeriod, FWidgetActiveTimerDelegate TickFunction)
{
	TSharedRef<FActiveTimerHandle> ActiveTimerHandle = MakeShared<FActiveTimerHandle>(TickPeriod, TickFunction, FSlateApplicationBase::Get().GetCurrentTime() + TickPeriod);
	FSlateApplicationBase::Get().RegisterActiveTimer(ActiveTimerHandle);
	ActiveTimers.Add(ActiveTimerHandle);

	AddUpdateFlags(EWidgetUpdateFlags::NeedsActiveTimerUpdate);

	return ActiveTimerHandle;
}

void SWidget::UnRegisterActiveTimer(const TSharedRef<FActiveTimerHandle>& ActiveTimerHandle)
{
	if (FSlateApplicationBase::IsInitialized())
	{
		FSlateApplicationBase::Get().UnRegisterActiveTimer(ActiveTimerHandle);
		ActiveTimers.Remove(ActiveTimerHandle);

		if (ActiveTimers.Num() == 0)
		{
			RemoveUpdateFlags(EWidgetUpdateFlags::NeedsActiveTimerUpdate);
		}
	}
}

void SWidget::ExecuteActiveTimers(double CurrentTime, float DeltaTime)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(SWidget::ExecuteActiveTimers);
	
	// loop over the registered tick handles and execute them, removing them if necessary.
	for (int32 i = 0; i < ActiveTimers.Num();)
	{
		EActiveTimerReturnType Result = ActiveTimers[i]->ExecuteIfPending(CurrentTime, DeltaTime);
		if (Result == EActiveTimerReturnType::Continue)
		{
			++i;
		}
		else
		{
			// Possible that execution unregistered the timer 
			if (ActiveTimers.IsValidIndex(i))
			{
				if (FSlateApplicationBase::IsInitialized())
				{
					FSlateApplicationBase::Get().UnRegisterActiveTimer(ActiveTimers[i]);
				}
				ActiveTimers.RemoveAt(i);
			}
		}
	}

	if (ActiveTimers.Num() == 0)
	{
		RemoveUpdateFlags(EWidgetUpdateFlags::NeedsActiveTimerUpdate);
	}
}

namespace Private
{
	TSharedPtr<FSlateMouseEventsMetaData> FindOrAddMouseEventsMetaData(SWidget* Widget)
	{
		TSharedPtr<FSlateMouseEventsMetaData> Data = Widget->GetMetaData<FSlateMouseEventsMetaData>();
		if (!Data)
		{
			Data = MakeShared<FSlateMouseEventsMetaData>();
			Widget->AddMetadata(Data.ToSharedRef());
		}
		return Data;
	}
}

void SWidget::SetOnMouseButtonDown(FPointerEventHandler EventHandler)
{
	Private::FindOrAddMouseEventsMetaData(this)->MouseButtonDownHandle = EventHandler;
}

void SWidget::SetOnMouseButtonUp(FPointerEventHandler EventHandler)
{
	Private::FindOrAddMouseEventsMetaData(this)->MouseButtonUpHandle = EventHandler;
}

void SWidget::SetOnMouseMove(FPointerEventHandler EventHandler)
{
	Private::FindOrAddMouseEventsMetaData(this)->MouseMoveHandle = EventHandler;
}

void SWidget::SetOnMouseDoubleClick(FPointerEventHandler EventHandler)
{
	Private::FindOrAddMouseEventsMetaData(this)->MouseDoubleClickHandle = EventHandler;
}

void SWidget::SetOnMouseEnter(FNoReplyPointerEventHandler EventHandler)
{
	Private::FindOrAddMouseEventsMetaData(this)->MouseEnterHandler = EventHandler;
}

void SWidget::SetOnMouseLeave(FSimpleNoReplyPointerEventHandler EventHandler)
{
	Private::FindOrAddMouseEventsMetaData(this)->MouseLeaveHandler = EventHandler;
}

void SWidget::AddMetadataInternal(const TSharedRef<ISlateMetaData>& AddMe)
{
	MetaData.Add(AddMe);

#if UE_WITH_SLATE_DEBUG_FIND_WIDGET_REFLECTION_METADATA || UE_SLATE_TRACE_ENABLED
	if (AddMe->IsOfType<FReflectionMetaData>())
	{
#if UE_WITH_SLATE_DEBUG_FIND_WIDGET_REFLECTION_METADATA
		TSharedRef<FReflectionMetaData> Reflection = StaticCastSharedRef<FReflectionMetaData>(AddMe);
		if (Reflection->Name == FindWidgetMetaData::WidgeName && Reflection->Asset.Get() && Reflection->Asset.Get()->GetFName() == FindWidgetMetaData::AssetName)
		{
			FindWidgetMetaData::FoundWidget = this;
		}
#endif
#if UE_SLATE_TRACE_ENABLED
		UE_TRACE_SLATE_WIDGET_DEBUG_INFO(this);
#endif
	}
#endif
}

#if WITH_ACCESSIBILITY
TSharedRef<FSlateAccessibleWidget> SWidget::CreateAccessibleWidget()
{
	return MakeShareable<FSlateAccessibleWidget>(new FSlateAccessibleWidget(AsShared()));
}

void SWidget::SetAccessibleBehavior(EAccessibleBehavior InBehavior, const TAttribute<FText>& InText, EAccessibleType AccessibleType)
{
	EAccessibleBehavior& Behavior = (AccessibleType == EAccessibleType::Main) ? AccessibleBehavior : AccessibleSummaryBehavior;

	if (InBehavior == EAccessibleBehavior::Custom)
	{
		TWidgetSparseAnnotation<TAttribute<FText>>& AccessibleTextAnnotation = (AccessibleType == EAccessibleType::Main) ? AccessibleText : AccessibleSummaryText;
		AccessibleTextAnnotation.FindOrAdd(this) = InText;
	}
	else if (Behavior == EAccessibleBehavior::Custom)
	{
		TWidgetSparseAnnotation<TAttribute<FText>>& AccessibleTextAnnotation = (AccessibleType == EAccessibleType::Main) ? AccessibleText : AccessibleSummaryText;
		AccessibleTextAnnotation.Remove(this);
	}

	if (Behavior != InBehavior)
	{
		const bool bWasAccessible = Behavior != EAccessibleBehavior::NotAccessible;
		Behavior = InBehavior;
		if (AccessibleType == EAccessibleType::Main && bWasAccessible != (Behavior != EAccessibleBehavior::NotAccessible))
		{
			FSlateApplicationBase::Get().GetAccessibleMessageHandler()->MarkDirty();
		}
	}
}

void SWidget::SetCanChildrenBeAccessible(bool InCanChildrenBeAccessible)
{
	if (bCanChildrenBeAccessible != InCanChildrenBeAccessible)
	{
		bCanChildrenBeAccessible = InCanChildrenBeAccessible;
		FSlateApplicationBase::Get().GetAccessibleMessageHandler()->MarkDirty();
	}
}

FText SWidget::GetAccessibleText(EAccessibleType AccessibleType) const
{
	const EAccessibleBehavior Behavior = (AccessibleType == EAccessibleType::Main) ? AccessibleBehavior : AccessibleSummaryBehavior;
	const EAccessibleBehavior OtherBehavior = (AccessibleType == EAccessibleType::Main) ? AccessibleSummaryBehavior : AccessibleBehavior;

	switch (Behavior)
	{
	case EAccessibleBehavior::Custom:
	{
		const TAttribute<FText>* Text = (AccessibleType == EAccessibleType::Main) ? AccessibleText.Find(this) : AccessibleSummaryText.Find(this);
		return Text->Get(FText::GetEmpty());
	}
	case EAccessibleBehavior::Summary:
		return GetAccessibleSummary();
	case EAccessibleBehavior::ToolTip:
	{
		//TODO should use GetToolTip
		if (TSharedPtr<FSlateToolTipMetaData> Data = GetMetaData<FSlateToolTipMetaData>())
		{
			if (TSharedPtr<IToolTip> ToolTip = Data->ToolTip.Get())
			{
				if (ToolTip && !ToolTip->IsEmpty())
				{
					return ToolTip->GetContentWidget()->GetAccessibleText(EAccessibleType::Main);
				}
			}
		}
		break;
	}
	case EAccessibleBehavior::Auto:
		// Auto first checks if custom text was set. This should never happen with user-defined values as custom should be
		// used instead in that case - however, this will be used for widgets with special default text such as TextBlocks.
		// If no text is found, then it will attempt to use the other variable's text, so that a developer can do things like
		// leave Summary on Auto, set Main to Custom, and have Summary automatically use Main's value without having to re-type it.
		TOptional<FText> DefaultText = GetDefaultAccessibleText(AccessibleType);
		if (DefaultText.IsSet())
		{
			return DefaultText.GetValue();
		}
		switch (OtherBehavior)
		{
		case EAccessibleBehavior::Custom:
		case EAccessibleBehavior::ToolTip:
			return GetAccessibleText(AccessibleType == EAccessibleType::Main ? EAccessibleType::Summary : EAccessibleType::Main);
		case EAccessibleBehavior::NotAccessible:
		case EAccessibleBehavior::Summary:
			return GetAccessibleSummary();
		}
		break;
	}
	return FText::GetEmpty();
}

TOptional<FText> SWidget::GetDefaultAccessibleText(EAccessibleType AccessibleType) const
{
	return TOptional<FText>();
}

FText SWidget::GetAccessibleSummary() const
{
	FTextBuilder Builder;
	FChildren* Children = const_cast<SWidget*>(this)->GetChildren();
	if (Children)
	{
		for (int32 i = 0; i < Children->Num(); ++i)
		{
			FText Text = Children->GetChildAt(i)->GetAccessibleText(EAccessibleType::Summary);
			if (!Text.IsEmpty())
			{
				Builder.AppendLine(Text);
			}
		}
	}
	return Builder.ToText();
}

bool SWidget::IsAccessible() const
{
	if (AccessibleBehavior == EAccessibleBehavior::NotAccessible)
	{
		return false;
	}

	TSharedPtr<SWidget> Parent = GetParentWidget();
	while (Parent.IsValid())
	{
		if (!Parent->CanChildrenBeAccessible())
		{
			return false;
		}
		Parent = Parent->GetParentWidget();
	}
	return true;
}

EAccessibleBehavior SWidget::GetAccessibleBehavior(EAccessibleType AccessibleType) const
{
	return AccessibleType == EAccessibleType::Main ? AccessibleBehavior : AccessibleSummaryBehavior;
}

bool SWidget::CanChildrenBeAccessible() const
{
	return bCanChildrenBeAccessible;
}

#endif

#if SLATE_CULL_WIDGETS

bool SWidget::IsChildWidgetCulled(const FSlateRect& MyCullingRect, const FArrangedWidget& ArrangedChild) const
{
	// If we've enabled global invalidation it's safe to run the culling logic and just 'stop' drawing
	// a widget, that widget has to be given an opportunity to paint, as wlel as all its children, the
	// only correct way is to remove the widget from the tree, or to change the visibility of it.
	if (GSlateIsOnFastUpdatePath)
	{
		return false;
	}

	// We add some slack fill to the culling rect to deal with the common occurrence
	// of widgets being larger than their root level widget is.  Happens when nested child widgets
	// inflate their rendering bounds to render beyond their parent (the child of this panel doing the culling), 
	// or using render transforms.  In either case, it introduces offsets to a bounding volume we don't 
	// actually know about or track in slate, so we have have two choices.
	//    1) Don't cull, set SLATE_CULL_WIDGETS to 0.
	//    2) Cull with a slack fill amount users can adjust.
	const FSlateRect CullingRectWithSlack = MyCullingRect.ScaleBy(GCullingSlackFillPercent);

	// 1) We check if the rendered bounding box overlaps with the culling rect.  Which is so that
	//    a render transformed element is never culled if it would have been visible to the user.
	if (FSlateRect::DoRectanglesIntersect(CullingRectWithSlack, ArrangedChild.Geometry.GetRenderBoundingRect()))
	{
		return false;
	}

	// 2) We also check the layout bounding box to see if it overlaps with the culling rect.  The
	//    reason for this is a bit more nuanced.  Suppose you dock a widget on the screen on the side
	//    and you want have it animate in and out of the screen.  Even though the layout transform 
	//    keeps the widget on the screen, the render transform alone would have caused it to be culled
	//    and therefore not ticked or painted.  The best way around this for now seems to be to simply
	//    check both rects to see if either one is overlapping the culling volume.
	if (FSlateRect::DoRectanglesIntersect(CullingRectWithSlack, ArrangedChild.Geometry.GetLayoutBoundingRect()))
	{
		return false;
	}

	// There's a special condition if the widget's clipping state is set does not intersect with clipping bounds, they in effect
	// will be setting a new culling rect, so let them pass being culling from this step.
	if (ArrangedChild.Widget->GetClipping() == EWidgetClipping::ClipToBoundsWithoutIntersecting)
	{
		return false;
	}

	return true;
}

#endif

#undef UE_WITH_SLATE_DEBUG_FIND_WIDGET_REFLECTION_METADATA