// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/ws/proxy_window.h"

#include <memory>

#include "base/run_loop.h"
#include "services/ws/client_root_test_helper.h"
#include "services/ws/window_service_test_setup.h"
#include "services/ws/window_tree.h"
#include "services/ws/window_tree_test_helper.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/aura/client/aura_constants.h"
#include "ui/aura/mus/client_surface_embedder.h"
#include "ui/events/event.h"
#include "ui/events/event_constants.h"
#include "ui/events/test/event_generator.h"
#include "ui/wm/core/easy_resize_window_targeter.h"

namespace ws {

TEST(ProxyWindow, FindTargetForWindowWithEasyResizeTargeter) {
  WindowServiceTestSetup setup;
  std::unique_ptr<wm::EasyResizeWindowTargeter> easy_resize_window_targeter =
      std::make_unique<wm::EasyResizeWindowTargeter>(
          gfx::Insets(-10, -10, -10, -10), gfx::Insets(-10, -10, -10, -10));
  setup.root()->SetEventTargeter(std::move(easy_resize_window_targeter));
  aura::Window* top_level =
      setup.window_tree_test_helper()->NewTopLevelWindow();
  ASSERT_TRUE(top_level);
  top_level->SetBounds(gfx::Rect(100, 200, 200, 200));
  top_level->Show();
  ui::MouseEvent mouse_event(ui::ET_MOUSE_PRESSED, gfx::Point(105, 195),
                             gfx::Point(105, 195), base::TimeTicks::Now(),
                             /* flags */ 0,
                             /* changed_button_flags_ */ 0);

  // Even though the mouse is not over |top_level| it should be returned as the
  // target because EasyResizeWindowTargeter enlarges the hit area.
  EXPECT_EQ(top_level, setup.root()->targeter()->FindTargetForEvent(
                           setup.root(), &mouse_event));

  // Repeat with a location outside the extended hit region and ensure
  // |top_level| is not returned.
  ui::MouseEvent mouse_event2(ui::ET_MOUSE_PRESSED, gfx::Point(5, 5),
                              gfx::Point(5, 5), base::TimeTicks::Now(),
                              /* flags */ 0,
                              /* changed_button_flags_ */ 0);
  EXPECT_NE(top_level, setup.root()->targeter()->FindTargetForEvent(
                           setup.root(), &mouse_event2));
}

TEST(ProxyWindow, FindTargetForWindowWithResizeInset) {
  WindowServiceTestSetup setup;

  aura::Window* top_level =
      setup.window_tree_test_helper()->NewTopLevelWindow();
  ASSERT_TRUE(top_level);
  const gfx::Rect top_level_bounds(100, 200, 200, 200);
  top_level->SetBounds(top_level_bounds);
  top_level->Show();

  aura::Window* child_window = setup.window_tree_test_helper()->NewWindow();
  ASSERT_TRUE(child_window);
  top_level->AddChild(child_window);
  child_window->SetBounds(gfx::Rect(top_level_bounds.size()));
  child_window->Show();

  const int kInset = 4;
  // Target an event at the resize inset area.
  gfx::Point click_point =
      top_level_bounds.left_center() + gfx::Vector2d(kInset / 2, 0);
  // With no resize inset set yet, the event should go to the child window.
  ui::MouseEvent mouse_event(ui::ET_MOUSE_PRESSED, click_point, click_point,
                             base::TimeTicks::Now(),
                             /* flags */ 0,
                             /* changed_button_flags_ */ 0);
  EXPECT_EQ(child_window, setup.root()->targeter()->FindTargetForEvent(
                              setup.root(), &mouse_event));

  // With the resize inset, the event should go to the toplevel.
  top_level->SetProperty(aura::client::kResizeHandleInset, kInset);
  ui::MouseEvent mouse_event_2(ui::ET_MOUSE_PRESSED, click_point, click_point,
                               base::TimeTicks::Now(),
                               /* flags */ 0,
                               /* changed_button_flags_ */ 0);
  EXPECT_EQ(top_level, setup.root()->targeter()->FindTargetForEvent(
                           setup.root(), &mouse_event_2));
}

TEST(ProxyWindow, ShouldSendPinchEventFromTouchpads) {
  WindowServiceTestSetup setup;

  aura::Window* top_level =
      setup.window_tree_test_helper()->NewTopLevelWindow();
  ASSERT_TRUE(top_level);
  const gfx::Rect top_level_bounds(100, 200, 200, 200);
  top_level->SetBounds(top_level_bounds);
  top_level->Show();

  setup.changes()->clear();

  ui::test::EventGenerator event_generator(top_level->GetRootWindow());

  // The pinch zoom from touchscreen -- that shouldn't be sent; the gesture
  // recognition in the client will create them.
  const gfx::Point points[] = {gfx::Point(110, 210), gfx::Point(190, 290)};
  const gfx::Vector2d deltas[] = {gfx::Vector2d(20, 20),
                                  gfx::Vector2d(-20, -20)};
  const int delay_fingers[] = {0, 10};
  event_generator.GestureMultiFingerScrollWithDelays(
      2, points, deltas, delay_fingers, delay_fingers, 0, 10);
  bool has_input_event = false;
  for (auto& c : *setup.changes()) {
    if (c.type != CHANGE_TYPE_INPUT_EVENT)
      continue;
    has_input_event = true;
    EXPECT_LE(ui::ET_TOUCH_RELEASED, c.event_action);
    EXPECT_GE(ui::ET_TOUCH_CANCELLED, c.event_action);
  }
  EXPECT_TRUE(has_input_event);

  setup.changes()->clear();

  // The pinch event from touchpad, this should be sent since the gesture
  // recognition isn't involved.
  ui::GestureEventDetails details(ui::ET_GESTURE_PINCH_BEGIN);
  details.set_device_type(ui::GestureDeviceType::DEVICE_TOUCHPAD);
  ui::GestureEvent pinch_event(110, 220, 0 /* flags */, base::TimeTicks::Now(),
                               details);
  ignore_result(
      setup.aura_test_helper()->event_sink()->OnEventFromSource(&pinch_event));
  auto iter = FirstChangeOfType(*setup.changes(), CHANGE_TYPE_INPUT_EVENT);
  ASSERT_NE(iter, setup.changes()->end());
  EXPECT_EQ(ui::ET_GESTURE_PINCH_BEGIN, iter->event_action);
}

}  // namespace ws
