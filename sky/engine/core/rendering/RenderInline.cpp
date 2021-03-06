/*
 * Copyright (C) 1999 Lars Knoll (knoll@kde.org)
 *           (C) 1999 Antti Koivisto (koivisto@kde.org)
 * Copyright (C) 2003, 2004, 2005, 2006, 2007, 2008, 2009 Apple Inc. All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 */

#include "sky/engine/config.h"
#include "sky/engine/core/rendering/RenderInline.h"

#include "sky/engine/core/dom/StyleEngine.h"
#include "sky/engine/core/page/Chrome.h"
#include "sky/engine/core/page/Page.h"
#include "sky/engine/core/rendering/GraphicsContextAnnotator.h"
#include "sky/engine/core/rendering/HitTestResult.h"
#include "sky/engine/core/rendering/InlineTextBox.h"
#include "sky/engine/core/rendering/RenderBlock.h"
#include "sky/engine/core/rendering/RenderGeometryMap.h"
#include "sky/engine/core/rendering/RenderLayer.h"
#include "sky/engine/core/rendering/RenderView.h"
#include "sky/engine/core/rendering/style/StyleInheritedData.h"
#include "sky/engine/platform/geometry/FloatQuad.h"
#include "sky/engine/platform/geometry/TransformState.h"
#include "sky/engine/platform/graphics/GraphicsContext.h"

namespace blink {

struct SameSizeAsRenderInline : public RenderBoxModelObject {
    virtual ~SameSizeAsRenderInline() { }
    RenderObjectChildList m_children;
    RenderLineBoxList m_lineBoxes;
};

COMPILE_ASSERT(sizeof(RenderInline) == sizeof(SameSizeAsRenderInline), RenderInline_should_stay_small);

RenderInline::RenderInline(Element* element)
    : RenderBoxModelObject(element)
{
}

RenderInline* RenderInline::createAnonymous(Document* document)
{
    RenderInline* renderer = new RenderInline(0);
    renderer->setDocumentForAnonymous(document);
    return renderer;
}

void RenderInline::willBeDestroyed()
{
    // Make sure to destroy anonymous children first while they are still connected to the rest of the tree, so that they will
    // properly dirty line boxes that they are removed from.  Effects that do :before/:after only on hover could crash otherwise.
    children()->destroyLeftoverChildren();

    if (!documentBeingDestroyed()) {
        if (firstLineBox()) {
            // We can't wait for RenderBoxModelObject::destroy to clear the selection,
            // because by then we will have nuked the line boxes.
            // FIXME: The FrameSelection should be responsible for this when it
            // is notified of DOM mutations.
            if (isSelectionBorder())
                view()->clearSelection();

            // If line boxes are contained inside a root, that means we're an inline.
            // In that case, we need to remove all the line boxes so that the parent
            // lines aren't pointing to deleted children. If the first line box does
            // not have a parent that means they are either already disconnected or
            // root lines that can just be destroyed without disconnecting.
            if (firstLineBox()->parent()) {
                for (InlineFlowBox* box = firstLineBox(); box; box = box->nextLineBox())
                    box->remove();
            }
        } else if (parent())
            parent()->dirtyLinesFromChangedChild(this);
    }

    m_lineBoxes.deleteLineBoxes();

    RenderBoxModelObject::willBeDestroyed();
}

void RenderInline::updateFromStyle()
{
    RenderBoxModelObject::updateFromStyle();

    // FIXME: Is this still needed. Was needed for run-ins, since run-in is considered a block display type.
    setInline(true);

    // FIXME: Support transforms and reflections on inline flows someday.
    setHasTransform(false);
}

void RenderInline::styleDidChange(StyleDifference diff, const RenderStyle* oldStyle)
{
    RenderBoxModelObject::styleDidChange(diff, oldStyle);

    if (!alwaysCreateLineBoxes()) {
        RenderStyle* newStyle = style();
        bool alwaysCreateLineBoxesNew = hasSelfPaintingLayer() || hasBoxDecorationBackground() || newStyle->hasPadding() || newStyle->hasMargin() || newStyle->hasOutline();
        if (oldStyle && alwaysCreateLineBoxesNew) {
            dirtyLineBoxes(false);
            setNeedsLayoutAndFullPaintInvalidation();
        }
        setAlwaysCreateLineBoxes(alwaysCreateLineBoxesNew);
    }
}

void RenderInline::updateAlwaysCreateLineBoxes(bool fullLayout)
{
    // Once we have been tainted once, just assume it will happen again. This way effects like hover highlighting that change the
    // background color will only cause a layout on the first rollover.
    if (alwaysCreateLineBoxes())
        return;

    RenderStyle* parentStyle = parent()->style();
    RenderInline* parentRenderInline = parent()->isRenderInline() ? toRenderInline(parent()) : 0;
    bool alwaysCreateLineBoxesNew = (parentRenderInline && parentRenderInline->alwaysCreateLineBoxes())
        || (parentRenderInline && parentStyle->verticalAlign() != BASELINE)
        || style()->verticalAlign() != BASELINE
        || style()->textEmphasisMark() != TextEmphasisMarkNone
        || !parentStyle->font().fontMetrics().hasIdenticalAscentDescentAndLineGap(style()->font().fontMetrics())
        || parentStyle->lineHeight() != style()->lineHeight();

    if (!alwaysCreateLineBoxesNew && document().styleEngine()->usesFirstLineRules()) {
        // Have to check the first line style as well.
        parentStyle = parent()->style(true);
        RenderStyle* childStyle = style(true);
        alwaysCreateLineBoxesNew = !parentStyle->font().fontMetrics().hasIdenticalAscentDescentAndLineGap(childStyle->font().fontMetrics())
        || childStyle->verticalAlign() != BASELINE
        || parentStyle->lineHeight() != childStyle->lineHeight();
    }

    if (alwaysCreateLineBoxesNew) {
        if (!fullLayout)
            dirtyLineBoxes(false);
        setAlwaysCreateLineBoxes();
    }
}

LayoutRect RenderInline::localCaretRect(InlineBox* inlineBox, int, LayoutUnit* extraWidthToEndOfLine)
{
    if (firstChild()) {
        // This condition is possible if the RenderInline is at an editing boundary,
        // i.e. the VisiblePosition is:
        //   <RenderInline editingBoundary=true>|<RenderText> </RenderText></RenderInline>
        // FIXME: need to figure out how to make this return a valid rect, note that
        // there are no line boxes created in the above case.
        return LayoutRect();
    }

    ASSERT_UNUSED(inlineBox, !inlineBox);

    if (extraWidthToEndOfLine)
        *extraWidthToEndOfLine = 0;

    LayoutRect caretRect = localCaretRectForEmptyElement(borderAndPaddingWidth(), 0);

    if (InlineBox* firstBox = firstLineBox())
        caretRect.moveBy(roundedLayoutPoint(firstBox->topLeft()));

    return caretRect;
}

void RenderInline::addChild(RenderObject* newChild, RenderObject* beforeChild)
{
    RenderBoxModelObject::addChild(newChild, beforeChild);
    newChild->setNeedsLayoutAndPrefWidthsRecalcAndFullPaintInvalidation();
}

void RenderInline::paint(PaintInfo& paintInfo, const LayoutPoint& paintOffset)
{
    ANNOTATE_GRAPHICS_CONTEXT(paintInfo, this);
    m_lineBoxes.paint(this, paintInfo, paintOffset);
}

template<typename GeneratorContext>
void RenderInline::generateLineBoxRects(GeneratorContext& yield) const
{
    if (!alwaysCreateLineBoxes())
        generateCulledLineBoxRects(yield, this);
    else if (InlineFlowBox* curr = firstLineBox()) {
        for (; curr; curr = curr->nextLineBox())
            yield(FloatRect(curr->topLeft(), curr->size()));
    } else
        yield(FloatRect());
}

template<typename GeneratorContext>
void RenderInline::generateCulledLineBoxRects(GeneratorContext& yield, const RenderInline* container) const
{
    if (!culledInlineFirstLineBox()) {
        yield(FloatRect());
        return;
    }

    for (RenderObject* curr = firstChild(); curr; curr = curr->nextSibling()) {
        if (curr->isFloatingOrOutOfFlowPositioned())
            continue;

        // We want to get the margin box in the inline direction, and then use our font ascent/descent in the block
        // direction (aligned to the root box's baseline).
        if (curr->isBox()) {
            RenderBox* currBox = toRenderBox(curr);
            if (currBox->inlineBoxWrapper()) {
                RootInlineBox& rootBox = currBox->inlineBoxWrapper()->root();
                int logicalTop = rootBox.logicalTop() + (rootBox.renderer().style(rootBox.isFirstLineStyle())->font().fontMetrics().ascent() - container->style(rootBox.isFirstLineStyle())->font().fontMetrics().ascent());
                int logicalHeight = container->style(rootBox.isFirstLineStyle())->font().fontMetrics().height();
                yield(FloatRect(currBox->inlineBoxWrapper()->x() - currBox->marginLeft(), logicalTop, (currBox->width() + currBox->marginWidth()).toFloat(), logicalHeight));
            }
        } else if (curr->isRenderInline()) {
            // If the child doesn't need line boxes either, then we can recur.
            RenderInline* currInline = toRenderInline(curr);
            if (!currInline->alwaysCreateLineBoxes())
                currInline->generateCulledLineBoxRects(yield, container);
            else {
                for (InlineFlowBox* childLine = currInline->firstLineBox(); childLine; childLine = childLine->nextLineBox()) {
                    RootInlineBox& rootBox = childLine->root();
                    int logicalTop = rootBox.logicalTop() + (rootBox.renderer().style(rootBox.isFirstLineStyle())->font().fontMetrics().ascent() - container->style(rootBox.isFirstLineStyle())->font().fontMetrics().ascent());
                    int logicalHeight = container->style(rootBox.isFirstLineStyle())->font().fontMetrics().height();
                    yield(FloatRect(childLine->x() - childLine->marginLogicalLeft(),
                        logicalTop,
                        childLine->logicalWidth() + childLine->marginLogicalLeft() + childLine->marginLogicalRight(),
                        logicalHeight));
                }
            }
        } else if (curr->isText()) {
            RenderText* currText = toRenderText(curr);
            for (InlineTextBox* childText = currText->firstTextBox(); childText; childText = childText->nextTextBox()) {
                RootInlineBox& rootBox = childText->root();
                int logicalTop = rootBox.logicalTop() + (rootBox.renderer().style(rootBox.isFirstLineStyle())->font().fontMetrics().ascent() - container->style(rootBox.isFirstLineStyle())->font().fontMetrics().ascent());
                int logicalHeight = container->style(rootBox.isFirstLineStyle())->font().fontMetrics().height();
                yield(FloatRect(childText->x(), logicalTop, childText->logicalWidth(), logicalHeight));
            }
        }
    }
}

namespace {

class AbsoluteRectsGeneratorContext {
public:
    AbsoluteRectsGeneratorContext(Vector<IntRect>& rects, const LayoutPoint& accumulatedOffset)
        : m_rects(rects)
        , m_accumulatedOffset(accumulatedOffset) { }

    void operator()(const FloatRect& rect)
    {
        IntRect intRect = enclosingIntRect(rect);
        intRect.move(m_accumulatedOffset.x(), m_accumulatedOffset.y());
        m_rects.append(intRect);
    }
private:
    Vector<IntRect>& m_rects;
    const LayoutPoint& m_accumulatedOffset;
};

} // unnamed namespace

void RenderInline::absoluteRects(Vector<IntRect>& rects, const LayoutPoint& accumulatedOffset) const
{
    AbsoluteRectsGeneratorContext context(rects, accumulatedOffset);
    generateLineBoxRects(context);
}


namespace {

class AbsoluteQuadsGeneratorContext {
public:
    AbsoluteQuadsGeneratorContext(const RenderInline* renderer, Vector<FloatQuad>& quads)
        : m_quads(quads)
        , m_geometryMap()
    {
        m_geometryMap.pushMappingsToAncestor(renderer, 0);
    }

    void operator()(const FloatRect& rect)
    {
        m_quads.append(m_geometryMap.absoluteRect(rect));
    }
private:
    Vector<FloatQuad>& m_quads;
    RenderGeometryMap m_geometryMap;
};

} // unnamed namespace

void RenderInline::absoluteQuads(Vector<FloatQuad>& quads) const
{
    AbsoluteQuadsGeneratorContext context(this, quads);
    generateLineBoxRects(context);
}

LayoutUnit RenderInline::offsetLeft() const
{
    LayoutPoint topLeft;
    if (InlineBox* firstBox = firstLineBoxIncludingCulling())
        topLeft = flooredLayoutPoint(firstBox->topLeft());
    return adjustedPositionRelativeToOffsetParent(topLeft).x();
}

LayoutUnit RenderInline::offsetTop() const
{
    LayoutPoint topLeft;
    if (InlineBox* firstBox = firstLineBoxIncludingCulling())
        topLeft = flooredLayoutPoint(firstBox->topLeft());
    return adjustedPositionRelativeToOffsetParent(topLeft).y();
}

static LayoutUnit computeMargin(const RenderInline* renderer, const Length& margin)
{
    if (margin.isAuto())
        return 0;
    if (margin.isFixed())
        return margin.value();
    if (margin.isPercent())
        return minimumValueForLength(margin, std::max<LayoutUnit>(0, renderer->containingBlock()->availableLogicalWidth()));
    return 0;
}

LayoutUnit RenderInline::marginLeft() const
{
    return computeMargin(this, style()->marginLeft());
}

LayoutUnit RenderInline::marginRight() const
{
    return computeMargin(this, style()->marginRight());
}

LayoutUnit RenderInline::marginTop() const
{
    return computeMargin(this, style()->marginTop());
}

LayoutUnit RenderInline::marginBottom() const
{
    return computeMargin(this, style()->marginBottom());
}

LayoutUnit RenderInline::marginStart(const RenderStyle* otherStyle) const
{
    return computeMargin(this, style()->marginStartUsing(otherStyle ? otherStyle : style()));
}

LayoutUnit RenderInline::marginEnd(const RenderStyle* otherStyle) const
{
    return computeMargin(this, style()->marginEndUsing(otherStyle ? otherStyle : style()));
}

LayoutUnit RenderInline::marginBefore(const RenderStyle* otherStyle) const
{
    return computeMargin(this, style()->marginBeforeUsing(otherStyle ? otherStyle : style()));
}

LayoutUnit RenderInline::marginAfter(const RenderStyle* otherStyle) const
{
    return computeMargin(this, style()->marginAfterUsing(otherStyle ? otherStyle : style()));
}

const char* RenderInline::renderName() const
{
    if (isRelPositioned())
        return "RenderInline (relative positioned)";
    if (isAnonymous())
        return "RenderInline (generated)";
    return "RenderInline";
}

bool RenderInline::nodeAtPoint(const HitTestRequest& request, HitTestResult& result,
                                const HitTestLocation& locationInContainer, const LayoutPoint& accumulatedOffset, HitTestAction hitTestAction)
{
    return m_lineBoxes.hitTest(this, request, result, locationInContainer, accumulatedOffset, hitTestAction);
}

namespace {

class HitTestCulledInlinesGeneratorContext {
public:
    HitTestCulledInlinesGeneratorContext(Region& region, const HitTestLocation& location) : m_intersected(false), m_region(region), m_location(location) { }
    void operator()(const FloatRect& rect)
    {
        m_intersected = m_intersected || m_location.intersects(rect);
        m_region.unite(enclosingIntRect(rect));
    }
    bool intersected() const { return m_intersected; }
private:
    bool m_intersected;
    Region& m_region;
    const HitTestLocation& m_location;
};

} // unnamed namespace

bool RenderInline::hitTestCulledInline(const HitTestRequest& request, HitTestResult& result, const HitTestLocation& locationInContainer, const LayoutPoint& accumulatedOffset)
{
    ASSERT(result.isRectBasedTest() && !alwaysCreateLineBoxes());
    if (!visibleToHitTestRequest(request))
        return false;

    HitTestLocation tmpLocation(locationInContainer, -toLayoutSize(accumulatedOffset));

    Region regionResult;
    HitTestCulledInlinesGeneratorContext context(regionResult, tmpLocation);
    generateCulledLineBoxRects(context, this);

    if (context.intersected()) {
        updateHitTestResult(result, tmpLocation.point());
        // We can not use addNodeToRectBasedTestResult to determine if we fully enclose the hit-test area
        // because it can only handle rectangular targets.
        result.addNodeToRectBasedTestResult(node(), request, locationInContainer);
        return regionResult.contains(tmpLocation.boundingBox());
    }
    return false;
}

PositionWithAffinity RenderInline::positionForPoint(const LayoutPoint& point)
{
    // FIXME(sky): Now that we don't have continuations, can this whole function just be the following?
    // return containingBlock()->positionForPoint(point);

    // FIXME: Does not deal with relative positioned inlines (should it?)
    RenderBlock* cb = containingBlock();
    if (firstLineBox()) {
        // This inline actually has a line box.  We must have clicked in the border/padding of one of these boxes.  We
        // should try to find a result by asking our containing block.
        return cb->positionForPoint(point);
    }

    // Translate the coords from the pre-anonymous block to the post-anonymous block.
    return RenderBoxModelObject::positionForPoint(point);
}

namespace {

class LinesBoundingBoxGeneratorContext {
public:
    LinesBoundingBoxGeneratorContext(FloatRect& rect) : m_rect(rect) { }
    void operator()(const FloatRect& rect)
    {
        m_rect.uniteIfNonZero(rect);
    }
private:
    FloatRect& m_rect;
};

} // unnamed namespace

IntRect RenderInline::linesBoundingBox() const
{
    if (!alwaysCreateLineBoxes()) {
        ASSERT(!firstLineBox());
        FloatRect floatResult;
        LinesBoundingBoxGeneratorContext context(floatResult);
        generateCulledLineBoxRects(context, this);
        return enclosingIntRect(floatResult);
    }

    IntRect result;

    // See <rdar://problem/5289721>, for an unknown reason the linked list here is sometimes inconsistent, first is non-zero and last is zero.  We have been
    // unable to reproduce this at all (and consequently unable to figure ot why this is happening).  The assert will hopefully catch the problem in debug
    // builds and help us someday figure out why.  We also put in a redundant check of lastLineBox() to avoid the crash for now.
    ASSERT(!firstLineBox() == !lastLineBox());  // Either both are null or both exist.
    if (firstLineBox() && lastLineBox()) {
        // Return the width of the minimal left side and the maximal right side.
        float logicalLeftSide = 0;
        float logicalRightSide = 0;
        for (InlineFlowBox* curr = firstLineBox(); curr; curr = curr->nextLineBox()) {
            if (curr == firstLineBox() || curr->logicalLeft() < logicalLeftSide)
                logicalLeftSide = curr->logicalLeft();
            if (curr == firstLineBox() || curr->logicalRight() > logicalRightSide)
                logicalRightSide = curr->logicalRight();
        }

        float x = logicalLeftSide;
        float y = firstLineBox()->y();
        float width = logicalRightSide - logicalLeftSide;
        float height = lastLineBox()->logicalBottom() - y;
        result = enclosingIntRect(FloatRect(x, y, width, height));
    }

    return result;
}

InlineBox* RenderInline::culledInlineFirstLineBox() const
{
    for (RenderObject* curr = firstChild(); curr; curr = curr->nextSibling()) {
        if (curr->isFloatingOrOutOfFlowPositioned())
            continue;

        // We want to get the margin box in the inline direction, and then use our font ascent/descent in the block
        // direction (aligned to the root box's baseline).
        if (curr->isBox())
            return toRenderBox(curr)->inlineBoxWrapper();
        if (curr->isRenderInline()) {
            RenderInline* currInline = toRenderInline(curr);
            InlineBox* result = currInline->firstLineBoxIncludingCulling();
            if (result)
                return result;
        } else if (curr->isText()) {
            RenderText* currText = toRenderText(curr);
            if (currText->firstTextBox())
                return currText->firstTextBox();
        }
    }
    return 0;
}

InlineBox* RenderInline::culledInlineLastLineBox() const
{
    for (RenderObject* curr = lastChild(); curr; curr = curr->previousSibling()) {
        if (curr->isFloatingOrOutOfFlowPositioned())
            continue;

        // We want to get the margin box in the inline direction, and then use our font ascent/descent in the block
        // direction (aligned to the root box's baseline).
        if (curr->isBox())
            return toRenderBox(curr)->inlineBoxWrapper();
        if (curr->isRenderInline()) {
            RenderInline* currInline = toRenderInline(curr);
            InlineBox* result = currInline->lastLineBoxIncludingCulling();
            if (result)
                return result;
        } else if (curr->isText()) {
            RenderText* currText = toRenderText(curr);
            if (currText->lastTextBox())
                return currText->lastTextBox();
        }
    }
    return 0;
}

LayoutRect RenderInline::culledInlineVisualOverflowBoundingBox() const
{
    FloatRect floatResult;
    LinesBoundingBoxGeneratorContext context(floatResult);
    generateCulledLineBoxRects(context, this);
    LayoutRect result(enclosingLayoutRect(floatResult));
    for (RenderObject* curr = firstChild(); curr; curr = curr->nextSibling()) {
        if (curr->isFloatingOrOutOfFlowPositioned())
            continue;

        // For overflow we just have to propagate by hand and recompute it all.
        if (curr->isBox()) {
            RenderBox* currBox = toRenderBox(curr);
            if (!currBox->hasSelfPaintingLayer() && currBox->inlineBoxWrapper()) {
                LayoutRect logicalRect = currBox->visualOverflowRect();
                logicalRect.moveBy(currBox->location());
                result.uniteIfNonZero(logicalRect);
            }
        } else if (curr->isRenderInline()) {
            // If the child doesn't need line boxes either, then we can recur.
            RenderInline* currInline = toRenderInline(curr);
            if (!currInline->alwaysCreateLineBoxes())
                result.uniteIfNonZero(currInline->culledInlineVisualOverflowBoundingBox());
            else if (!currInline->hasSelfPaintingLayer())
                result.uniteIfNonZero(currInline->linesVisualOverflowBoundingBox());
        } else if (curr->isText()) {
            // FIXME; Overflow from text boxes is lost. We will need to cache this information in
            // InlineTextBoxes.
            RenderText* currText = toRenderText(curr);
            result.uniteIfNonZero(currText->linesVisualOverflowBoundingBox());
        }
    }
    return result;
}

LayoutRect RenderInline::linesVisualOverflowBoundingBox() const
{
    if (!alwaysCreateLineBoxes())
        return culledInlineVisualOverflowBoundingBox();

    if (!firstLineBox() || !lastLineBox())
        return LayoutRect();

    // Return the width of the minimal left side and the maximal right side.
    LayoutUnit logicalLeftSide = LayoutUnit::max();
    LayoutUnit logicalRightSide = LayoutUnit::min();
    for (InlineFlowBox* curr = firstLineBox(); curr; curr = curr->nextLineBox()) {
        logicalLeftSide = std::min(logicalLeftSide, curr->logicalLeftVisualOverflow());
        logicalRightSide = std::max(logicalRightSide, curr->logicalRightVisualOverflow());
    }

    RootInlineBox& firstRootBox = firstLineBox()->root();
    RootInlineBox& lastRootBox = lastLineBox()->root();

    LayoutUnit logicalTop = firstLineBox()->logicalTopVisualOverflow(firstRootBox.lineTop());
    LayoutUnit logicalWidth = logicalRightSide - logicalLeftSide;
    LayoutUnit logicalHeight = lastLineBox()->logicalBottomVisualOverflow(lastRootBox.lineBottom()) - logicalTop;

    LayoutRect rect(logicalLeftSide, logicalTop, logicalWidth, logicalHeight);
    return rect;
}

LayoutRect RenderInline::clippedOverflowRectForPaintInvalidation(const RenderLayerModelObject* paintInvalidationContainer, const PaintInvalidationState* paintInvalidationState) const
{
    if (!firstLineBoxIncludingCulling())
        return LayoutRect();

    LayoutRect paintInvalidationRect(linesVisualOverflowBoundingBox());
    bool hitPaintInvalidationContainer = false;

    // We need to add in the in-flow position offsets of any inlines (including us) up to our
    // containing block.
    RenderBlock* cb = containingBlock();
    for (const RenderObject* inlineFlow = this; inlineFlow && inlineFlow->isRenderInline() && inlineFlow != cb;
         inlineFlow = inlineFlow->parent()) {
        if (inlineFlow == paintInvalidationContainer) {
            hitPaintInvalidationContainer = true;
            break;
        }
        if (inlineFlow->style()->hasInFlowPosition() && inlineFlow->hasLayer())
            paintInvalidationRect.move(toRenderInline(inlineFlow)->layer()->offsetForInFlowPosition());
    }

    LayoutUnit outlineSize = style()->outlineSize();
    paintInvalidationRect.inflate(outlineSize);

    if (hitPaintInvalidationContainer || !cb)
        return paintInvalidationRect;

    if (cb->hasOverflowClip())
        cb->applyCachedClipAndScrollOffsetForPaintInvalidation(paintInvalidationRect);

    // FIXME: Passing paintInvalidationState directly to mapRectToPaintInvalidationBacking causes incorrect invalidations.
    // Should avoid slowRectMapping by properly adjusting paintInvalidationState. crbug.com/402994.
    ForceHorriblySlowRectMapping slowRectMapping(paintInvalidationState);
    cb->mapRectToPaintInvalidationBacking(paintInvalidationContainer, paintInvalidationRect, paintInvalidationState);

    if (outlineSize) {
        for (RenderObject* curr = firstChild(); curr; curr = curr->nextSibling()) {
            if (!curr->isText())
                paintInvalidationRect.unite(curr->rectWithOutlineForPaintInvalidation(paintInvalidationContainer, outlineSize));
        }
    }

    return paintInvalidationRect;
}

LayoutRect RenderInline::rectWithOutlineForPaintInvalidation(const RenderLayerModelObject* paintInvalidationContainer, LayoutUnit outlineWidth, const PaintInvalidationState* paintInvalidationState) const
{
    LayoutRect r(RenderBoxModelObject::rectWithOutlineForPaintInvalidation(paintInvalidationContainer, outlineWidth, paintInvalidationState));
    for (RenderObject* curr = firstChild(); curr; curr = curr->nextSibling()) {
        if (!curr->isText())
            r.unite(curr->rectWithOutlineForPaintInvalidation(paintInvalidationContainer, outlineWidth, paintInvalidationState));
    }
    return r;
}

void RenderInline::mapRectToPaintInvalidationBacking(const RenderLayerModelObject* paintInvalidationContainer, LayoutRect& rect, const PaintInvalidationState* paintInvalidationState) const
{
    if (paintInvalidationState && paintInvalidationState->canMapToContainer(paintInvalidationContainer)) {
        if (style()->hasInFlowPosition() && layer())
            rect.move(layer()->offsetForInFlowPosition());
        rect.move(paintInvalidationState->paintOffset());
        if (paintInvalidationState->isClipped())
            rect.intersect(paintInvalidationState->clipRect());
        return;
    }

    if (paintInvalidationContainer == this)
        return;

    bool containerSkipped;
    RenderObject* o = container(paintInvalidationContainer, &containerSkipped);
    if (!o)
        return;

    LayoutPoint topLeft = rect.location();

    if (style()->hasInFlowPosition() && layer()) {
        // Apply the in-flow position offset when invalidating a rectangle. The layer
        // is translated, but the render box isn't, so we need to do this to get the
        // right dirty rect. Since this is called from RenderObject::setStyle, the relative position
        // flag on the RenderObject has been cleared, so use the one on the style().
        topLeft += layer()->offsetForInFlowPosition();
    }

    // FIXME: We ignore the lightweight clipping rect that controls use, since if |o| is in mid-layout,
    // its controlClipRect will be wrong. For overflow clip we use the values cached by the layer.
    rect.setLocation(topLeft);
    if (o->hasOverflowClip()) {
        RenderBox* containerBox = toRenderBox(o);
        containerBox->applyCachedClipAndScrollOffsetForPaintInvalidation(rect);
        if (rect.isEmpty())
            return;
    }

    if (containerSkipped) {
        // If the paintInvalidationContainer is below o, then we need to map the rect into paintInvalidationContainer's coordinates.
        LayoutSize containerOffset = paintInvalidationContainer->offsetFromAncestorContainer(o);
        rect.move(-containerOffset);
        return;
    }

    o->mapRectToPaintInvalidationBacking(paintInvalidationContainer, rect, paintInvalidationState);
}

LayoutSize RenderInline::offsetFromContainer(const RenderObject* container, const LayoutPoint& point, bool* offsetDependsOnPoint) const
{
    ASSERT(container == this->container());

    LayoutSize offset;
    if (isRelPositioned())
        offset += offsetForInFlowPosition();

    if (container->hasOverflowClip())
        offset -= toRenderBox(container)->scrolledContentOffset();

    // FIXME(sky): Remove now that it's always false?
    if (offsetDependsOnPoint)
        *offsetDependsOnPoint = false;

    return offset;
}

void RenderInline::mapLocalToContainer(const RenderLayerModelObject* paintInvalidationContainer, TransformState& transformState, MapCoordinatesFlags mode, const PaintInvalidationState* paintInvalidationState) const
{
    if (paintInvalidationContainer == this)
        return;

    if (paintInvalidationState && paintInvalidationState->canMapToContainer(paintInvalidationContainer)) {
        LayoutSize offset = paintInvalidationState->paintOffset();
        if (style()->hasInFlowPosition() && layer())
            offset += layer()->offsetForInFlowPosition();
        transformState.move(offset);
        return;
    }

    bool containerSkipped;
    RenderObject* o = container(paintInvalidationContainer, &containerSkipped);
    if (!o)
        return;

    if (mode & ApplyContainerFlip && o->isBox()) {
        mode &= ~ApplyContainerFlip;
    }

    LayoutSize containerOffset = offsetFromContainer(o, roundedLayoutPoint(transformState.mappedPoint()));

    bool preserve3D = mode & UseTransforms && (o->style()->preserves3D() || style()->preserves3D());
    if (mode & UseTransforms && shouldUseTransformFromContainer(o)) {
        TransformationMatrix t;
        getTransformFromContainer(o, containerOffset, t);
        transformState.applyTransform(t, preserve3D ? TransformState::AccumulateTransform : TransformState::FlattenTransform);
    } else
        transformState.move(containerOffset.width(), containerOffset.height(), preserve3D ? TransformState::AccumulateTransform : TransformState::FlattenTransform);

    if (containerSkipped) {
        // There can't be a transform between paintInvalidationContainer and o, because transforms create containers, so it should be safe
        // to just subtract the delta between the paintInvalidationContainer and o.
        LayoutSize containerOffset = paintInvalidationContainer->offsetFromAncestorContainer(o);
        transformState.move(-containerOffset.width(), -containerOffset.height(), preserve3D ? TransformState::AccumulateTransform : TransformState::FlattenTransform);
        return;
    }

    o->mapLocalToContainer(paintInvalidationContainer, transformState, mode, paintInvalidationState);
}

void RenderInline::updateHitTestResult(HitTestResult& result, const LayoutPoint& point)
{
    if (result.innerNode())
        return;

    Node* n = node();
    LayoutPoint localPoint(point);
    if (n) {
        result.setInnerNode(n);
        if (!result.innerNonSharedNode())
            result.setInnerNonSharedNode(n);
        result.setLocalPoint(localPoint);
    }
}

void RenderInline::dirtyLineBoxes(bool fullLayout)
{
    if (fullLayout) {
        m_lineBoxes.deleteLineBoxes();
        return;
    }

    if (!alwaysCreateLineBoxes()) {
        // We have to grovel into our children in order to dirty the appropriate lines.
        for (RenderObject* curr = firstChild(); curr; curr = curr->nextSibling()) {
            if (curr->isFloatingOrOutOfFlowPositioned())
                continue;
            if (curr->isBox() && !curr->needsLayout()) {
                RenderBox* currBox = toRenderBox(curr);
                if (currBox->inlineBoxWrapper())
                    currBox->inlineBoxWrapper()->root().markDirty();
            } else if (!curr->selfNeedsLayout()) {
                if (curr->isRenderInline()) {
                    RenderInline* currInline = toRenderInline(curr);
                    for (InlineFlowBox* childLine = currInline->firstLineBox(); childLine; childLine = childLine->nextLineBox())
                        childLine->root().markDirty();
                } else if (curr->isText()) {
                    RenderText* currText = toRenderText(curr);
                    for (InlineTextBox* childText = currText->firstTextBox(); childText; childText = childText->nextTextBox())
                        childText->root().markDirty();
                }
            }
        }
    } else
        m_lineBoxes.dirtyLineBoxes();
}

void RenderInline::deleteLineBoxTree()
{
    m_lineBoxes.deleteLineBoxTree();
}

InlineFlowBox* RenderInline::createInlineFlowBox()
{
    return new InlineFlowBox(*this);
}

InlineFlowBox* RenderInline::createAndAppendInlineFlowBox()
{
    setAlwaysCreateLineBoxes();
    InlineFlowBox* flowBox = createInlineFlowBox();
    m_lineBoxes.appendLineBox(flowBox);
    return flowBox;
}

LayoutUnit RenderInline::lineHeight(bool firstLine, LineDirectionMode /*direction*/, LinePositionMode /*linePositionMode*/) const
{
    if (firstLine && document().styleEngine()->usesFirstLineRules()) {
        RenderStyle* s = style(firstLine);
        if (s != style())
            return s->computedLineHeight();
    }

    return style()->computedLineHeight();
}

int RenderInline::baselinePosition(FontBaseline baselineType, bool firstLine, LineDirectionMode direction, LinePositionMode linePositionMode) const
{
    ASSERT(linePositionMode == PositionOnContainingLine);
    const FontMetrics& fontMetrics = style(firstLine)->fontMetrics();
    return fontMetrics.ascent(baselineType) + (lineHeight(firstLine, direction, linePositionMode) - fontMetrics.height()) / 2;
}

LayoutSize RenderInline::offsetForInFlowPositionedInline(const RenderBox& child) const
{
    // FIXME: This function isn't right with mixed writing modes.

    ASSERT(isRelPositioned());
    if (!isRelPositioned())
        return LayoutSize();

    // When we have an enclosing relpositioned inline, we need to add in the offset of the first line
    // box from the rest of the content, but only in the cases where we know we're positioned
    // relative to the inline itself.

    LayoutSize logicalOffset;
    LayoutUnit inlinePosition;
    LayoutUnit blockPosition;
    if (firstLineBox()) {
        inlinePosition = LayoutUnit::fromFloatRound(firstLineBox()->logicalLeft());
        blockPosition = firstLineBox()->logicalTop();
    } else {
        inlinePosition = layer()->staticInlinePosition();
        blockPosition = layer()->staticBlockPosition();
    }

    // Per http://www.w3.org/TR/CSS2/visudet.html#abs-non-replaced-width an absolute positioned box
    // with a static position should locate itself as though it is a normal flow box in relation to
    // its containing block. If this relative-positioned inline has a negative offset we need to
    // compensate for it so that we align the positioned object with the edge of its containing block.
    if (child.style()->hasStaticInlinePosition())
        logicalOffset.setWidth(std::max(LayoutUnit(), -offsetForInFlowPosition().width()));
    else
        logicalOffset.setWidth(inlinePosition);

    if (!child.style()->hasStaticBlockPosition())
        logicalOffset.setHeight(blockPosition);

    return logicalOffset;
}

void RenderInline::imageChanged(WrappedImagePtr, const IntRect*)
{
    if (!parent())
        return;

    // FIXME: We can do better.
    setShouldDoFullPaintInvalidation(true);
}

namespace {

class AbsoluteRectsIgnoringEmptyRectsGeneratorContext : public AbsoluteRectsGeneratorContext {
public:
    AbsoluteRectsIgnoringEmptyRectsGeneratorContext(Vector<IntRect>& rects, const LayoutPoint& accumulatedOffset)
        : AbsoluteRectsGeneratorContext(rects, accumulatedOffset) { }

    void operator()(const FloatRect& rect)
    {
        if (!rect.isEmpty())
            AbsoluteRectsGeneratorContext::operator()(rect);
    }
};

} // unnamed namespace

void RenderInline::addFocusRingRects(Vector<IntRect>& rects, const LayoutPoint& additionalOffset, const RenderLayerModelObject* paintContainer) const
{
    AbsoluteRectsIgnoringEmptyRectsGeneratorContext context(rects, additionalOffset);
    generateLineBoxRects(context);

    addChildFocusRingRects(rects, additionalOffset, paintContainer);
}

namespace {

class AbsoluteLayoutRectsGeneratorContext {
public:
    AbsoluteLayoutRectsGeneratorContext(Vector<LayoutRect>& rects, const LayoutPoint& accumulatedOffset)
        : m_rects(rects)
        , m_accumulatedOffset(accumulatedOffset) { }

    void operator()(const FloatRect& rect)
    {
        LayoutRect layoutRect(rect);
        layoutRect.move(m_accumulatedOffset.x(), m_accumulatedOffset.y());
        m_rects.append(layoutRect);
    }
private:
    Vector<LayoutRect>& m_rects;
    const LayoutPoint& m_accumulatedOffset;
};

}

void RenderInline::paintOutline(PaintInfo& paintInfo, const LayoutPoint& paintOffset)
{
    RenderStyle* styleToUse = style();
    if (!styleToUse->hasOutline())
        return;

    if (styleToUse->outlineStyleIsAuto())
        return;

    if (styleToUse->outlineStyle() == BNONE)
        return;

    Vector<LayoutRect> rects;

    rects.append(LayoutRect());
    for (InlineFlowBox* curr = firstLineBox(); curr; curr = curr->nextLineBox()) {
        RootInlineBox& root = curr->root();
        LayoutUnit top = std::max<LayoutUnit>(root.lineTop(), curr->logicalTop());
        LayoutUnit bottom = std::min<LayoutUnit>(root.lineBottom(), curr->logicalBottom());
        rects.append(LayoutRect(curr->x(), top, curr->logicalWidth(), bottom - top));
    }
    rects.append(LayoutRect());

    Color outlineColor = resolveColor(styleToUse, CSSPropertyOutlineColor);
    bool useTransparencyLayer = outlineColor.hasAlpha();

    GraphicsContext* graphicsContext = paintInfo.context;
    if (useTransparencyLayer) {
        graphicsContext->beginTransparencyLayer(static_cast<float>(outlineColor.alpha()) / 255);
        outlineColor = Color(outlineColor.red(), outlineColor.green(), outlineColor.blue());
    }

    for (unsigned i = 1; i < rects.size() - 1; i++)
        paintOutlineForLine(graphicsContext, paintOffset, rects.at(i - 1), rects.at(i), rects.at(i + 1), outlineColor);

    if (useTransparencyLayer)
        graphicsContext->endLayer();
}

void RenderInline::paintOutlineForLine(GraphicsContext* graphicsContext, const LayoutPoint& paintOffset,
                                       const LayoutRect& lastline, const LayoutRect& thisline, const LayoutRect& nextline,
                                       const Color outlineColor)
{
    RenderStyle* styleToUse = style();
    int outlineWidth = styleToUse->outlineWidth();
    EBorderStyle outlineStyle = styleToUse->outlineStyle();

    bool antialias = shouldAntialiasLines(graphicsContext);

    int offset = style()->outlineOffset();

    LayoutRect box(LayoutPoint(paintOffset.x() + thisline.x() - offset, paintOffset.y() + thisline.y() - offset),
        LayoutSize(thisline.width() + offset, thisline.height() + offset));

    IntRect pixelSnappedBox = pixelSnappedIntRect(box);
    if (pixelSnappedBox.width() < 0 || pixelSnappedBox.height() < 0)
        return;
    IntRect pixelSnappedLastLine = pixelSnappedIntRect(paintOffset.x() + lastline.x(), 0, lastline.width(), 0);
    IntRect pixelSnappedNextLine = pixelSnappedIntRect(paintOffset.x() + nextline.x(), 0, nextline.width(), 0);

    // left edge
    drawLineForBoxSide(graphicsContext,
        pixelSnappedBox.x() - outlineWidth,
        pixelSnappedBox.y() - (lastline.isEmpty() || thisline.x() < lastline.x() || (lastline.maxX() - 1) <= thisline.x() ? outlineWidth : 0),
        pixelSnappedBox.x(),
        pixelSnappedBox.maxY() + (nextline.isEmpty() || thisline.x() <= nextline.x() || (nextline.maxX() - 1) <= thisline.x() ? outlineWidth : 0),
        BSLeft,
        outlineColor, outlineStyle,
        (lastline.isEmpty() || thisline.x() < lastline.x() || (lastline.maxX() - 1) <= thisline.x() ? outlineWidth : -outlineWidth),
        (nextline.isEmpty() || thisline.x() <= nextline.x() || (nextline.maxX() - 1) <= thisline.x() ? outlineWidth : -outlineWidth),
        antialias);

    // right edge
    drawLineForBoxSide(graphicsContext,
        pixelSnappedBox.maxX(),
        pixelSnappedBox.y() - (lastline.isEmpty() || lastline.maxX() < thisline.maxX() || (thisline.maxX() - 1) <= lastline.x() ? outlineWidth : 0),
        pixelSnappedBox.maxX() + outlineWidth,
        pixelSnappedBox.maxY() + (nextline.isEmpty() || nextline.maxX() <= thisline.maxX() || (thisline.maxX() - 1) <= nextline.x() ? outlineWidth : 0),
        BSRight,
        outlineColor, outlineStyle,
        (lastline.isEmpty() || lastline.maxX() < thisline.maxX() || (thisline.maxX() - 1) <= lastline.x() ? outlineWidth : -outlineWidth),
        (nextline.isEmpty() || nextline.maxX() <= thisline.maxX() || (thisline.maxX() - 1) <= nextline.x() ? outlineWidth : -outlineWidth),
        antialias);
    // upper edge
    if (thisline.x() < lastline.x())
        drawLineForBoxSide(graphicsContext,
            pixelSnappedBox.x() - outlineWidth,
            pixelSnappedBox.y() - outlineWidth,
            std::min(pixelSnappedBox.maxX() + outlineWidth, (lastline.isEmpty() ? 1000000 : pixelSnappedLastLine.x())),
            pixelSnappedBox.y(),
            BSTop, outlineColor, outlineStyle,
            outlineWidth,
            (!lastline.isEmpty() && paintOffset.x() + lastline.x() + 1 < pixelSnappedBox.maxX() + outlineWidth) ? -outlineWidth : outlineWidth,
            antialias);

    if (lastline.maxX() < thisline.maxX())
        drawLineForBoxSide(graphicsContext,
            std::max(lastline.isEmpty() ? -1000000 : pixelSnappedLastLine.maxX(), pixelSnappedBox.x() - outlineWidth),
            pixelSnappedBox.y() - outlineWidth,
            pixelSnappedBox.maxX() + outlineWidth,
            pixelSnappedBox.y(),
            BSTop, outlineColor, outlineStyle,
            (!lastline.isEmpty() && pixelSnappedBox.x() - outlineWidth < paintOffset.x() + lastline.maxX()) ? -outlineWidth : outlineWidth,
            outlineWidth, antialias);

    if (thisline.x() == thisline.maxX())
          drawLineForBoxSide(graphicsContext,
            pixelSnappedBox.x() - outlineWidth,
            pixelSnappedBox.y() - outlineWidth,
            pixelSnappedBox.maxX() + outlineWidth,
            pixelSnappedBox.y(),
            BSTop, outlineColor, outlineStyle,
            outlineWidth,
            outlineWidth,
            antialias);

    // lower edge
    if (thisline.x() < nextline.x())
        drawLineForBoxSide(graphicsContext,
            pixelSnappedBox.x() - outlineWidth,
            pixelSnappedBox.maxY(),
            std::min(pixelSnappedBox.maxX() + outlineWidth, !nextline.isEmpty() ? pixelSnappedNextLine.x() + 1 : 1000000),
            pixelSnappedBox.maxY() + outlineWidth,
            BSBottom, outlineColor, outlineStyle,
            outlineWidth,
            (!nextline.isEmpty() && paintOffset.x() + nextline.x() + 1 < pixelSnappedBox.maxX() + outlineWidth) ? -outlineWidth : outlineWidth,
            antialias);

    if (nextline.maxX() < thisline.maxX())
        drawLineForBoxSide(graphicsContext,
            std::max(!nextline.isEmpty() ? pixelSnappedNextLine.maxX() : -1000000, pixelSnappedBox.x() - outlineWidth),
            pixelSnappedBox.maxY(),
            pixelSnappedBox.maxX() + outlineWidth,
            pixelSnappedBox.maxY() + outlineWidth,
            BSBottom, outlineColor, outlineStyle,
            (!nextline.isEmpty() && pixelSnappedBox.x() - outlineWidth < paintOffset.x() + nextline.maxX()) ? -outlineWidth : outlineWidth,
            outlineWidth, antialias);

    if (thisline.x() == thisline.maxX())
          drawLineForBoxSide(graphicsContext,
            pixelSnappedBox.x() - outlineWidth,
            pixelSnappedBox.maxY(),
            pixelSnappedBox.maxX() + outlineWidth,
            pixelSnappedBox.maxY() + outlineWidth,
            BSBottom, outlineColor, outlineStyle,
            outlineWidth,
            outlineWidth,
            antialias);
}

} // namespace blink
