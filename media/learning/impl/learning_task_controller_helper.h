// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_LEARNING_IMPL_LEARNING_TASK_CONTROLLER_HELPER_H_
#define MEDIA_LEARNING_IMPL_LEARNING_TASK_CONTROLLER_HELPER_H_

#include <map>
#include <memory>

#include "base/callback.h"
#include "base/component_export.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequenced_task_runner.h"
#include "base/threading/sequence_bound.h"
#include "media/learning/common/learning_task_controller.h"
#include "media/learning/impl/feature_provider.h"

namespace media {
namespace learning {

class LearningTaskControllerHelperTest;
class RunOnDelete;

// Helper class for managing LabelledExamples that are constructed
// incrementally.  Keeps track of in-flight examples as they're added via
// BeginObservation, updated with features from a FeatureProvider, or given a
// TargetValue.  When examples are complete, it provides them to a callback for
// further processing.
//
// Since both the mojo LearningTaskController and LearningTaskControllerImpl
// will need to do almost exactly the same thing, this class handles the common
// logic for them.
class COMPONENT_EXPORT(LEARNING_IMPL) LearningTaskControllerHelper
    : public base::SupportsWeakPtr<LearningTaskControllerHelper> {
 public:
  // Callback to add labelled examples as training data.
  using AddExampleCB = base::RepeatingCallback<void(LabelledExample)>;

  // TODO(liberato): Consider making the FP not optional.
  LearningTaskControllerHelper(const LearningTask& task,
                               AddExampleCB add_example_cb,
                               SequenceBoundFeatureProvider feature_provider =
                                   SequenceBoundFeatureProvider());
  virtual ~LearningTaskControllerHelper();

  // See LearningTaskController::BeginObservation.
  LearningTaskController::SetTargetValueCB BeginObservation(
      FeatureVector features);

 private:
  // Record of an example that has been started by RecordObservedFeatures, but
  // not finished yet.
  struct PendingExample {
    // The example we're constructing.
    LabelledExample example;
    // Has the FeatureProvider added features?
    bool features_done = false;
    // Has the client added a TargetValue?
    // TODO(liberato): Should it provide a weight with the target value?
    bool target_done = false;
  };

  // [non-repeating int] = example
  using PendingExampleMap = std::map<int64_t, PendingExample>;

  // Called by the SetTargetCB for a particular example.  |example_id| is the
  // PendingExample id.  |run_on_delete| is an RAII class that will notify us
  // when it's deleted, so that we can clear the example out of the map.
  // |target_value| is the observed target for the example.
  // TODO(liberato): This should take a weight, too.
  void OnTargetValue(int64_t example_id,
                     std::unique_ptr<RunOnDelete> run_on_delete,
                     TargetValue target_value,
                     WeightType weight);

  // Called when the SetTargetValueCB is destroyed.
  void OnLabelCallbackDestroyed(int64_t example_id);

  // Called on any sequence when features are ready.  Will call OnFeatureReady
  // if called on |task_runner|, or will post to |task_runner|.
  static void OnFeaturesReadyTrampoline(
      scoped_refptr<base::SequencedTaskRunner> task_runner,
      base::WeakPtr<LearningTaskControllerHelper> weak_this,
      int64_t example_id,
      FeatureVector features);

  // Called when a new feature vector has been finished by |feature_provider_|,
  // if needed, to actually add the example.
  void OnFeaturesReady(int64_t example_id, FeatureVector features);

  // If |example| is finished, then send it to the LearningSession and remove it
  // from the map.  Otherwise, do nothing.
  void ProcessExampleIfFinished(PendingExampleMap::iterator example);

  // The task we'll add examples to.
  LearningTask task_;

  // Optional feature provider.
  SequenceBoundFeatureProvider feature_provider_;

  // Next arbitrary integer to be used in the map.
  int64_t next_example_id_ = 1;

  // All outstanding PendingExamples.
  PendingExampleMap pending_examples_;

  // While the handling of |pending_examples_| is an implementation detail, we
  // still let tests verify the map size, to help catch cases where we forget to
  // remove things from the map and slowly leak memory.
  size_t pending_example_count_for_testing() const {
    return pending_examples_.size();
  }

  scoped_refptr<base::SequencedTaskRunner> task_runner_;

  // Callback to which we'll send finished examples.
  AddExampleCB add_example_cb_;

  friend class LearningTaskControllerHelperTest;
};

}  // namespace learning
}  // namespace media

#endif  // MEDIA_LEARNING_IMPL_LEARNING_TASK_CONTROLLER_HELPER_H_
