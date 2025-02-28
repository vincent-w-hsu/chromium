// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/wm/splitview/split_view_drag_indicators.h"

#include "ash/shell.h"
#include "ash/test/ash_test_base.h"
#include "ash/wm/overview/overview_controller.h"
#include "ash/wm/overview/overview_grid.h"
#include "ash/wm/overview/overview_item.h"
#include "ash/wm/overview/overview_session.h"
#include "ash/wm/splitview/split_view_constants.h"
#include "ash/wm/splitview/split_view_controller.h"
#include "ash/wm/tablet_mode/tablet_mode_controller.h"
#include "services/ws/public/mojom/window_tree_constants.mojom.h"
#include "ui/aura/client/aura_constants.h"
#include "ui/aura/env.h"
#include "ui/aura/window.h"
#include "ui/events/test/event_generator.h"
#include "ui/views/widget/widget.h"

namespace ash {

class SplitViewDragIndicatorsTest : public AshTestBase {
 public:
  SplitViewDragIndicatorsTest() = default;
  ~SplitViewDragIndicatorsTest() override = default;

  void SetUp() override {
    AshTestBase::SetUp();

    // Ensure calls to EnableTabletModeWindowManager complete.
    base::RunLoop().RunUntilIdle();
    Shell::Get()->tablet_mode_controller()->EnableTabletModeWindowManager(true);
    base::RunLoop().RunUntilIdle();
  }

  void ToggleOverview() {
    auto* overview_controller = Shell::Get()->overview_controller();
    overview_controller->ToggleOverview();
    if (!overview_controller->IsSelecting()) {
      overview_session_ = nullptr;
      split_view_drag_indicators_ = nullptr;
      return;
    }

    overview_session_ = Shell::Get()->overview_controller()->overview_session();
    ASSERT_TRUE(overview_session_);
    split_view_drag_indicators_ =
        overview_session_->split_view_drag_indicators();
  }

  SplitViewController* split_view_controller() {
    return Shell::Get()->split_view_controller();
  }

  IndicatorState indicator_state() {
    DCHECK(split_view_drag_indicators_);
    return split_view_drag_indicators_->current_indicator_state();
  }

  bool IsPreviewAreaShowing() {
    return indicator_state() == IndicatorState::kPreviewAreaLeft ||
           indicator_state() == IndicatorState::kPreviewAreaRight;
  }

  OverviewItem* GetOverviewItemForWindow(aura::Window* window,
                                         int grid_index = 0) {
    auto& windows =
        overview_session_->grid_list_for_testing()[grid_index]->window_list();
    auto iter =
        std::find_if(windows.cbegin(), windows.cend(),
                     [window](const std::unique_ptr<OverviewItem>& item) {
                       return item->Contains(window);
                     });
    if (iter == windows.end())
      return nullptr;
    return iter->get();
  }

  float GetEdgeInset(int screen_width) const {
    return screen_width * kHighlightScreenPrimaryAxisRatio +
           kHighlightScreenEdgePaddingDp;
  }

  // Creates a window which cannot be snapped by splitview.
  std::unique_ptr<aura::Window> CreateUnsnappableWindow() {
    std::unique_ptr<aura::Window> window(CreateTestWindow());
    window->SetProperty(aura::client::kResizeBehaviorKey,
                        ws::mojom::kResizeBehaviorNone);
    return window;
  }

 protected:
  SplitViewDragIndicators* split_view_drag_indicators_ = nullptr;
  OverviewSession* overview_session_ = nullptr;

 private:
  DISALLOW_COPY_AND_ASSIGN(SplitViewDragIndicatorsTest);
};

TEST_F(SplitViewDragIndicatorsTest, Dragging) {
  Shell::Get()->aura_env()->set_throttle_input_on_resize_for_testing(false);
  UpdateDisplay("800x600");
  const int screen_width = 800;
  const float edge_inset = GetEdgeInset(screen_width);
  std::unique_ptr<aura::Window> right_window(CreateTestWindow());
  std::unique_ptr<aura::Window> left_window(CreateTestWindow());
  ui::test::EventGenerator* generator = GetEventGenerator();

  ToggleOverview();
  OverviewItem* left_item = GetOverviewItemForWindow(left_window.get());
  OverviewItem* right_item = GetOverviewItemForWindow(right_window.get());

  // The inset on each side of the screen which is a snap region. Items dragged
  // to and released under this region will get snapped.
  const int drag_offset = 5;
  const int drag_offset_snap_region = 48;
  const int minimum_drag_offset = 96;
  // The selector item has a margin which does not accept events. Inset any
  // event aimed at the selector items edge so events will reach it.
  const int item_inset = 20;

  // Check the two windows set up have a region which is under no snap region, a
  // region that is under the left snap region and a region that is under the
  // right snap region.
  ASSERT_GT(left_item->target_bounds().CenterPoint().x(), edge_inset);
  ASSERT_LT(left_item->target_bounds().origin().x() + item_inset, edge_inset);
  ASSERT_GT(right_item->target_bounds().right() - item_inset,
            screen_width - edge_inset);

  // Verify if the drag is not started in either snap region, the drag still
  // must move by |drag_offset| before split view acknowledges the drag (ie.
  // starts moving the selector item).
  generator->set_current_screen_location(
      gfx::ToRoundedPoint(left_item->target_bounds().CenterPoint()));
  generator->PressLeftButton();
  const gfx::RectF left_original_bounds = left_item->target_bounds();
  generator->MoveMouseBy(drag_offset - 1, 0);
  EXPECT_EQ(left_original_bounds, left_item->target_bounds());
  generator->MoveMouseBy(1, 0);
  EXPECT_NE(left_original_bounds, left_item->target_bounds());
  generator->ReleaseLeftButton();

  // Verify if the drag is started in the left snap region, the drag needs to
  // move by |drag_offset_snap_region| towards the right side of the screen
  // before split view acknowledges the drag (shows the preview area).
  ASSERT_TRUE(Shell::Get()->overview_controller()->IsSelecting());
  generator->set_current_screen_location(
      gfx::Point(left_item->target_bounds().origin().x() + item_inset,
                 left_item->target_bounds().CenterPoint().y()));
  generator->PressLeftButton();
  generator->MoveMouseBy(-drag_offset, 0);
  EXPECT_FALSE(IsPreviewAreaShowing());
  generator->MoveMouseBy(drag_offset_snap_region, 0);
  generator->MoveMouseBy(-minimum_drag_offset, 0);
  EXPECT_TRUE(IsPreviewAreaShowing());
  // Drag back to the middle before releasing so that we stay in overview mode
  // on release.
  generator->MoveMouseTo(
      gfx::ToRoundedPoint(left_original_bounds.CenterPoint()));
  generator->ReleaseLeftButton();

  // Verify if the drag is started in the right snap region, the drag needs to
  // move by |drag_offset_snap_region| towards the left side of the screen
  // before split view acknowledges the drag.
  ASSERT_TRUE(Shell::Get()->overview_controller()->IsSelecting());
  generator->set_current_screen_location(
      gfx::Point(right_item->target_bounds().right() - item_inset,
                 right_item->target_bounds().CenterPoint().y()));
  generator->PressLeftButton();
  generator->MoveMouseBy(drag_offset, 0);
  EXPECT_FALSE(IsPreviewAreaShowing());
  generator->MoveMouseBy(-drag_offset_snap_region, 0);
  generator->MoveMouseBy(minimum_drag_offset, 0);
  EXPECT_TRUE(IsPreviewAreaShowing());
}

// Verify the split view preview area becomes visible when expected.
TEST_F(SplitViewDragIndicatorsTest, PreviewAreaVisibility) {
  UpdateDisplay("800x600");
  const int screen_width = 800;
  const float edge_inset = GetEdgeInset(screen_width);
  std::unique_ptr<aura::Window> window(CreateTestWindow());
  ToggleOverview();

  // Verify the preview area is visible when |item|'s x is in the
  // range [0, edge_inset] or [screen_width - edge_inset - 1, screen_width].
  OverviewItem* item = GetOverviewItemForWindow(window.get());
  ASSERT_TRUE(item);
  const gfx::PointF start_location(item->target_bounds().CenterPoint());
  // Drag horizontally to avoid activating drag to close.
  const float y = start_location.y();
  overview_session_->InitiateDrag(item, start_location);
  EXPECT_FALSE(IsPreviewAreaShowing());
  overview_session_->Drag(item, gfx::PointF(edge_inset + 1, y));
  EXPECT_FALSE(IsPreviewAreaShowing());
  overview_session_->Drag(item, gfx::PointF(edge_inset, y));
  EXPECT_TRUE(IsPreviewAreaShowing());

  overview_session_->Drag(item, gfx::PointF(screen_width - edge_inset - 2, y));
  EXPECT_FALSE(IsPreviewAreaShowing());
  overview_session_->Drag(item, gfx::PointF(screen_width - edge_inset - 1, y));
  EXPECT_TRUE(IsPreviewAreaShowing());

  // Drag back to |start_location| before compeleting the drag, otherwise
  // |selector_time| will snap to the right and the system will enter splitview,
  // making |window_drag_controller()| nullptr.
  overview_session_->Drag(item, start_location);
  overview_session_->CompleteDrag(item, start_location);
  EXPECT_FALSE(IsPreviewAreaShowing());
}

// Verify that the preview area never shows up when dragging a unsnappable
// window.
TEST_F(SplitViewDragIndicatorsTest, PreviewAreaVisibilityUnsnappableWindow) {
  UpdateDisplay("800x600");
  const int screen_width = 800;
  std::unique_ptr<aura::Window> window(CreateUnsnappableWindow());
  ToggleOverview();

  OverviewItem* item = GetOverviewItemForWindow(window.get());
  const gfx::PointF start_location(item->target_bounds().CenterPoint());
  overview_session_->InitiateDrag(item, start_location);
  EXPECT_FALSE(IsPreviewAreaShowing());
  overview_session_->Drag(item, gfx::PointF(0.f, 1.f));
  EXPECT_FALSE(IsPreviewAreaShowing());
  overview_session_->Drag(item, gfx::PointF(screen_width, 1.f));
  EXPECT_FALSE(IsPreviewAreaShowing());

  overview_session_->CompleteDrag(item, start_location);
  EXPECT_FALSE(IsPreviewAreaShowing());
}

// Verify that the split view overview overlay has the expected state.
TEST_F(SplitViewDragIndicatorsTest, SplitViewDragIndicatorsState) {
  UpdateDisplay("800x600");
  const int screen_width = 800;
  const float edge_inset = GetEdgeInset(screen_width);
  std::unique_ptr<aura::Window> window1(CreateTestWindow());
  std::unique_ptr<aura::Window> window2(CreateTestWindow());
  ToggleOverview();

  // Verify that when are no snapped windows, the indicator is visible once
  // there is a long press or after the drag has started.
  OverviewItem* item = GetOverviewItemForWindow(window1.get());
  gfx::PointF start_location(item->target_bounds().CenterPoint());
  overview_session_->InitiateDrag(item, start_location);
  EXPECT_EQ(IndicatorState::kNone, indicator_state());
  overview_session_->StartSplitViewDragMode(start_location);
  EXPECT_EQ(IndicatorState::kDragArea, indicator_state());

  // Reset the gesture so we stay in overview mode.
  overview_session_->ResetDraggedWindowGesture();

  // Verify the indicator is visible once the item starts moving, and becomes a
  // preview area once we reach the left edge of the screen. Drag horizontal to
  // avoid activating drag to close.
  const float y_position = start_location.y();
  overview_session_->InitiateDrag(item, start_location);
  EXPECT_EQ(IndicatorState::kNone, indicator_state());
  overview_session_->Drag(item, gfx::PointF(edge_inset + 1, y_position));
  EXPECT_EQ(IndicatorState::kDragArea, indicator_state());
  overview_session_->Drag(item, gfx::PointF(edge_inset, y_position));
  EXPECT_EQ(IndicatorState::kPreviewAreaLeft, indicator_state());

  // Snap window to the left.
  overview_session_->CompleteDrag(item, gfx::PointF(edge_inset, y_position));
  ASSERT_TRUE(split_view_controller()->IsSplitViewModeActive());
  ASSERT_EQ(SplitViewController::LEFT_SNAPPED,
            split_view_controller()->state());

  // Verify that when there is a left snapped window, dragging an item to the
  // right will show the right preview area.
  item = GetOverviewItemForWindow(window2.get());
  start_location = item->target_bounds().CenterPoint();
  overview_session_->InitiateDrag(item, start_location);
  EXPECT_EQ(IndicatorState::kNone, indicator_state());
  overview_session_->Drag(item, gfx::PointF(screen_width - 1, y_position));
  EXPECT_EQ(IndicatorState::kPreviewAreaRight, indicator_state());
  overview_session_->CompleteDrag(item, start_location);
}

// Verify that the split view drag indicator is shown when expected when
// attempting to drag a unsnappable window.
TEST_F(SplitViewDragIndicatorsTest,
       SplitViewDragIndicatorVisibilityUnsnappableWindow) {
  std::unique_ptr<aura::Window> unsnappable_window(CreateUnsnappableWindow());
  ToggleOverview();

  OverviewItem* item = GetOverviewItemForWindow(unsnappable_window.get());
  gfx::PointF start_location(item->target_bounds().CenterPoint());
  overview_session_->InitiateDrag(item, start_location);
  overview_session_->StartSplitViewDragMode(start_location);
  EXPECT_EQ(IndicatorState::kCannotSnap, indicator_state());
  const gfx::PointF end_location1(0.f, 0.f);
  overview_session_->Drag(item, end_location1);
  EXPECT_EQ(IndicatorState::kCannotSnap, indicator_state());
  overview_session_->CompleteDrag(item, end_location1);
  EXPECT_EQ(IndicatorState::kNone, indicator_state());
}

// Verify when the split view drag indicators state changes, the expected
// indicators will become visible or invisible.
TEST_F(SplitViewDragIndicatorsTest, SplitViewDragIndicatorsVisibility) {
  auto indicator = std::make_unique<SplitViewDragIndicators>();

  auto to_int = [](IndicatorType type) { return static_cast<int>(type); };

  // Helper function to which checks that all indicator types passed in |mask|
  // are visible, and those that are not are not visible.
  auto check_helper = [](SplitViewDragIndicators* svdi, int mask) {
    const std::vector<IndicatorType> types = {
        IndicatorType::kLeftHighlight, IndicatorType::kLeftText,
        IndicatorType::kRightHighlight, IndicatorType::kRightText};
    for (auto type : types) {
      if ((static_cast<int>(type) & mask) > 0)
        EXPECT_TRUE(svdi->GetIndicatorTypeVisibilityForTesting(type));
      else
        EXPECT_FALSE(svdi->GetIndicatorTypeVisibilityForTesting(type));
    }
  };

  // Check each state has the correct views displayed. Pass and empty point as
  // the location since there is no need to reparent the widget. Verify that
  // nothing is shown in the none state.
  indicator->SetIndicatorState(IndicatorState::kNone, gfx::Point());
  check_helper(indicator.get(), 0);

  const int all = to_int(IndicatorType::kLeftHighlight) |
                  to_int(IndicatorType::kLeftText) |
                  to_int(IndicatorType::kRightHighlight) |
                  to_int(IndicatorType::kRightText);
  // Verify that everything is visible in the dragging and cannot snap states.
  indicator->SetIndicatorState(IndicatorState::kDragArea, gfx::Point());
  check_helper(indicator.get(), all);
  indicator->SetIndicatorState(IndicatorState::kCannotSnap, gfx::Point());
  check_helper(indicator.get(), all);

  // Verify that only one highlight shows up for the preview area states.
  indicator->SetIndicatorState(IndicatorState::kPreviewAreaLeft, gfx::Point());
  check_helper(indicator.get(), to_int(IndicatorType::kLeftHighlight));
  indicator->SetIndicatorState(IndicatorState::kPreviewAreaRight, gfx::Point());
  check_helper(indicator.get(), to_int(IndicatorType::kRightHighlight));
}

// Verify that the split view drag indicators widget reparents when starting a
// drag on a different display.
TEST_F(SplitViewDragIndicatorsTest, SplitViewDragIndicatorsWidgetReparenting) {
  // Add two displays and one window on each display.
  UpdateDisplay("600x600,600x600");
  // DisplayConfigurationObserver enables mirror mode when tablet mode is
  // enabled. Disable mirror mode to test multiple displays.
  display_manager()->SetMirrorMode(display::MirrorMode::kOff, base::nullopt);
  base::RunLoop().RunUntilIdle();

  auto root_windows = Shell::Get()->GetAllRootWindows();
  ASSERT_EQ(2u, root_windows.size());

  const gfx::Rect primary_screen_bounds(0, 0, 600, 600);
  const gfx::Rect secondary_screen_bounds(600, 0, 600, 600);
  auto primary_screen_window(CreateTestWindow(primary_screen_bounds));
  auto secondary_screen_window(CreateTestWindow(secondary_screen_bounds));

  ToggleOverview();

  // Select an item on the primary display and verify the drag indicators
  // widget's parent is the primary root window.
  OverviewItem* item = GetOverviewItemForWindow(primary_screen_window.get());
  gfx::PointF start_location(item->target_bounds().CenterPoint());
  overview_session_->InitiateDrag(item, start_location);
  overview_session_->Drag(item, gfx::PointF(100.f, start_location.y()));
  EXPECT_EQ(IndicatorState::kDragArea, indicator_state());
  EXPECT_EQ(root_windows[0], overview_session_->split_view_drag_indicators()
                                 ->widget_->GetNativeView()
                                 ->GetRootWindow());
  // Drag the item in a way that neither opens the window nor activates
  // splitview mode.
  overview_session_->Drag(item,
                          gfx::PointF(primary_screen_bounds.CenterPoint()));
  overview_session_->CompleteDrag(
      item, gfx::PointF(primary_screen_bounds.CenterPoint()));
  ASSERT_TRUE(Shell::Get()->overview_controller()->IsSelecting());
  ASSERT_FALSE(split_view_controller()->IsSplitViewModeActive());

  // Select an item on the secondary display and verify the indicators widget
  // has reparented to the secondary root window.
  item = GetOverviewItemForWindow(secondary_screen_window.get(), 1);
  start_location = item->target_bounds().CenterPoint();
  overview_session_->InitiateDrag(item, start_location);
  overview_session_->Drag(item, gfx::PointF(800.f, start_location.y()));
  EXPECT_EQ(IndicatorState::kDragArea, indicator_state());
  EXPECT_EQ(root_windows[1], overview_session_->split_view_drag_indicators()
                                 ->widget_->GetNativeView()
                                 ->GetRootWindow());
  overview_session_->CompleteDrag(item, start_location);
}

}  // namespace ash