// Copyright Epic Games, Inc. All Rights Reserved.

#include "Widgets/SCompoundWidget.h"
#include "Types/PaintArgs.h"
#include "Layout/ArrangedChildren.h"
#include "Layout/LayoutUtils.h"
#include "SlateGlobals.h"

DECLARE_CYCLE_STAT(TEXT("Child Paint"), STAT_ChildPaint, STATGROUP_SlateVeryVerbose);


/**
 * @brief 绘制复合widget及其子widget
 * 
 * @param Args 绘制参数
 * @param AllottedGeometry widget的几何形状
 * @param MyCullingRect 用于剔除的边界
 * @param OutDrawElements 绘制元素列表
 * @param LayerId widget的层级
 * @param InWidgetStyle widget的样式
 * @param bParentEnabled 父widget是否启用
 * @return int32 新的层级
 * 
 * 例如，假设你有一个复合widget，它包含一个按钮和一个文本框。当你调用复合widget的OnPaint方法时，它会首先安排按钮和文本框的布局，然后分别调用按钮和文本框的Paint方法来绘制按钮和文本框。
 */
int32 SCompoundWidget::OnPaint( const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled ) const
{
	// 创建一个FArrangedChildren对象，用于存储可见的子widget。
	// A CompoundWidget just draws its children
	FArrangedChildren ArrangedChildren(EVisibility::Visible);
	{
		// 调用ArrangeChildren方法来安排子widget的布局。
		this->ArrangeChildren(AllottedGeometry, ArrangedChildren);

		/*
		ArrangeChildren是一个虚函数，它在每个widget类中都可能被重写。
		这个函数的作用是根据AllottedGeometry（widget的几何形状）来安排子widget的位置和大小，并将结果存储在ArrangedChildren中。
		*/
	}

	// 如果有可见的子widget，就绘制子widget。
	// There may be zero elements in this array if our child collapsed/hidden
	if( ArrangedChildren.Num() > 0 )
	{
		check( ArrangedChildren.Num() == 1 );// SCompoundWidget 类设计为只能包含一个子控件。
		FArrangedWidget& TheChild = ArrangedChildren[0];

		FWidgetStyle CompoundedWidgetStyle = FWidgetStyle(InWidgetStyle)
			.BlendColorAndOpacityTint(ColorAndOpacity.Get())
			.SetForegroundColor( GetForegroundColor() );

		int32 Layer = 0;
		{
#if WITH_VERY_VERBOSE_SLATE_STATS
			SCOPE_CYCLE_COUNTER(STAT_ChildPaint);
#endif
			/** 调用子widget的Paint方法来绘制子widget。 */
			Layer = TheChild.Widget->Paint( Args.WithNewParent(this), TheChild.Geometry, MyCullingRect, OutDrawElements, LayerId + 1, CompoundedWidgetStyle, ShouldBeEnabled( bParentEnabled ) );
		}
		return Layer;
	}
	return LayerId;
}

FChildren* SCompoundWidget::GetChildren()
{
	return &ChildSlot;
}


FVector2D SCompoundWidget::ComputeDesiredSize( float ) const
{
	EVisibility ChildVisibility = ChildSlot.GetWidget()->GetVisibility();
	if ( ChildVisibility != EVisibility::Collapsed )
	{
		return ChildSlot.GetWidget()->GetDesiredSize() + ChildSlot.SlotPadding.Get().GetDesiredSize();
	}
	
	return FVector2D::ZeroVector;
}

void SCompoundWidget::OnArrangeChildren( const FGeometry& AllottedGeometry, FArrangedChildren& ArrangedChildren ) const
{
	ArrangeSingleChild(GSlateFlowDirection, AllottedGeometry, ArrangedChildren, ChildSlot, ContentScale);
}

FSlateColor SCompoundWidget::GetForegroundColor() const
{
	return ForegroundColor.Get();
}

SCompoundWidget::SCompoundWidget()
	: ChildSlot(this)
	, ContentScale( FVector2D(1.0f,1.0f) )
	, ColorAndOpacity( FLinearColor::White )
	, ForegroundColor( FSlateColor::UseForeground() )
{
	SET_TAG_FOR_TRACE(SCompoundWidget)
}

void SCompoundWidget::SetVisibility( TAttribute<EVisibility> InVisibility )
{
	SWidget::SetVisibility( InVisibility );
}
