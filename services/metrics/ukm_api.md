# URL-Keyed Metrics API

This describes how to write client code to collect UKM data. Before you add new metrics, you should file a proposal.  See go/ukm for more information.

## Document your metrics in ukm.xml

Any events and metrics you collect need to be documented in [//tools/metrics/ukm/ukm.xml](https://cs.chromium.org/chromium/src/tools/metrics/ukm/ukm.xml)

Important information to include:

* Metric owners: This the email of someone who can answer questions about how this metric is recorded, what it means, and how it should be used. Can include multiple people.
* A description of the event about which you are recording details, including when the event will be recorded.
* For each metric in the event: a description of the data and what it means.
* The unit should be included in the description, along with possible output values.
* If an event will only happen once per Navigation, it can be marked singular="true".

### Example
```
<event name="Goat.Teleported">
  <owner>teleporter@chromium.org</owner>
  <summary>
    Recorded when a page teleports a goat.
  </summary>
  <metric name="Duration">
    <summary>
      How long it took to teleport, in ns.
    </summary>
  </metric>
  <metric name="Mass">
    <summary>
      The mass of the teleported goat, in kg, rounded to the nearest multiple of 10.
    </summary>
  </metric>
</event>
```

### Controlling the Aggregation of Metrics

Control of which metrics are included in the History table is done via the same
[`tools/metrics/ukm/ukm.xml`](https://cs.chromium.org/chromium/src/tools/metrics/ukm/ukm.xml)
file in the Chromium codebase. To have a metric aggregated, `<aggregation>` and
`<history>` tags need to be added.

```
<event name="Goat.Teleported">
  <metric name="Duration">
    ...
    <aggregation>
      <history>
        <index fields="profile.country"/>
        <statistics>
          <quantiles type="std-percentiles"/>
        </statistics>
      </history>
    </aggregation>
    ...
  </metric>
</event>
```

Supported statistic types are:

*   `<quantiles type="std-percentiles"/>`: Calculates the "standard percentiles"
    for the values which are 1, 5, 10, 25, 50, 75, 90, 95, and 99%ile.
*   `<enumeration/>`: Calculates the proportions of all values individually. The
    proportions indicate the relative frequency of each bucket and are
    calculated independently for each metric over each aggregation. The
    proportions will sum to 1.0 for an enumeration that emits only one result
    per page-load if it emits anything at all. An enumeration emitted more than
    once on a page will result in proportions that total greater than 1.0.

There can also be one or more `index` tags which define additional aggregation
keys. These are a comma-separated list of keys that is appended to the standard
set. These additional keys are optional but, if present, are always present
together. In other words, "fields=profile.county,profile.form_factory" will
cause all the standard aggregations plus each with *both* country *and*
form_factor but **not** with all the standard aggregations (see above) plus only
one of them. If individual and combined versions are desired, use multiple index
tags.

Currently supported additional index fields are:

*   `profile.country`
*   `profile.form_factor`

## Get UkmRecorder instance

In order to record UKM events, your code needs a UkmRecorder object, defined by [//services/metrics/public/cpp/ukm_recorder.h](https://cs.chromium.org/chromium/src/services/metrics/public/cpp/ukm_recorder.h)

There are two main ways of getting a UkmRecorder instance.

1) Use ukm::UkmRecorder::Get().  This currently only works from the Browser process.

2) Use a service connector and get a UkmRecorder.

```
std::unique_ptr<ukm::UkmRecorder> ukm_recorder =
    ukm::UkmRecorder::Create(context()->connector());
ukm::builders::MyEvent(source_id)
    .SetMyMetric(metric_value)
    .Record(ukm_recorder.get());
```

## Get a ukm::SourceId

UKM identifies navigations by their source ID and you'll need to associate and ID with your event in order to tie it to a main frame URL.  Preferrably, get an existing ID for the navigation from another object.

The main methods for doing this are using one of the following methods:

```
ukm::SourceId source_id = GetSourceIdForWebContentsDocument(web_contents);
ukm::SourceId source_id = ukm::ConvertToSourceId(
    navigation_handle->GetNavigationId(), ukm::SourceIdType::NAVIGATION_ID);
```

Currently, however, the code for passing these IDs around is incomplete so you may need to temporarily create your own IDs and associate the URL with them. However we currently prefer that this method is not used, and if you need to setup the URL yourself, please email us first at ukm-team@google.com.
Example:

```
ukm::SourceId source_id = ukm::UkmRecorder::GetNewSourceID();
ukm_recorder->UpdateSourceURL(source_id, main_frame_url);
```

You will also need to add your class as a friend of UkmRecorder in order to use this private API.

## Create some events

Helper objects for recording your event are generated from the descriptions in ukm.xml.  You can use them like so:

```
#include "services/metrics/public/cpp/ukm_builders.h"

void OnGoatTeleported() {
  ...
  ukm::builders::Goat_Teleported(source_id)
      .SetDuration(duration.InNanoseconds())
      .SetMass(RoundedToMultiple(mass_kg, 10))
      .Record(ukm_recorder);
}
```

If the event name in the XML contains a period (`.`), it is replaced with an underscore (`_`) in the method name.

## Check that it works

Build chromium and run it with '--force-enable-metrics-reporting'.  Trigger your event and check chrome://ukm to make sure the data was recorded correctly.

## Unit testing

You can pass your code a TestUkmRecorder (see [//components/ukm/test_ukm_recorder.h](https://cs.chromium.org/chromium/src/components/ukm/test_ukm_recorder.h)) and then use the methods it provides to test that your data records correctly.
