// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/webauth/authenticator_impl.h"

#include <array>
#include <string>
#include <utility>
#include <vector>

#include "base/base64url.h"
#include "base/bind.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/metrics/histogram_macros.h"
#include "base/rand_util.h"
#include "base/strings/string_piece.h"
#include "base/timer/timer.h"
#include "build/build_config.h"
#include "content/browser/bad_message.h"
#include "content/browser/webauth/authenticator_type_converters.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/content_browser_client.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/content_client.h"
#include "content/public/common/content_features.h"
#include "content/public/common/origin_util.h"
#include "content/public/common/service_manager_connection.h"
#include "crypto/sha2.h"
#include "device/base/features.h"
#include "device/bluetooth/bluetooth_adapter_factory.h"
#include "device/fido/attestation_statement.h"
#include "device/fido/authenticator_selection_criteria.h"
#include "device/fido/ctap_get_assertion_request.h"
#include "device/fido/ctap_make_credential_request.h"
#include "device/fido/features.h"
#include "device/fido/fido_authenticator.h"
#include "device/fido/fido_constants.h"
#include "device/fido/fido_transport_protocol.h"
#include "device/fido/get_assertion_request_handler.h"
#include "device/fido/make_credential_request_handler.h"
#include "device/fido/public_key_credential_descriptor.h"
#include "device/fido/public_key_credential_params.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "net/cert/asn1_util.h"
#include "net/der/input.h"
#include "net/der/parse_values.h"
#include "net/der/parser.h"
#include "net/url_request/url_request_context.h"
#include "net/url_request/url_request_context_getter.h"
#include "services/service_manager/public/cpp/connector.h"
#include "url/url_constants.h"
#include "url/url_util.h"

#if defined(OS_MACOSX)
#include "device/fido/mac/authenticator.h"
#endif

#if defined(OS_WIN)
#include "device/fido/win/authenticator.h"
#endif

namespace content {

namespace client_data {
const char kCreateType[] = "webauthn.create";
const char kGetType[] = "webauthn.get";
const char kU2fSignType[] = "navigator.id.getAssertion";
const char kU2fRegisterType[] = "navigator.id.finishEnrollment";
}  // namespace client_data

namespace {

constexpr char kCryptotokenOrigin[] =
    "chrome-extension://kmendfapggjehodndflmmgagdbamhnfd";

// AttestationPromptResult enumerates events related to attestation prompts.
// These values are recorded in an UMA histogram and so should not be
// reassigned.
enum class AttestationPromptResult {
  // kQueried indicates that the embedder was queried in order to determine
  // whether attestation information should be returned to the origin.
  kQueried = 0,
  // kTimeout indicates that a timeout occured while awaiting the result of an
  // attestation query.
  kTimeout = 1,
  // kAllowed indicates that the query to the embedder was resolved positively.
  // (E.g. the user clicked to allow, or the embedded allowed immediately by
  // policy.)
  kAllowed = 2,
  // kBlocked indicates that the query to the embedder was resolved negatively.
  // (E.g. the user clicked to block, or closed the dialog.)
  kBlocked = 3,
  // kAbandoned indications that the user closed the tab or navigated away while
  // the attestation prompt was showing.
  kAbandoned = 4,
  kMaxValue = kAbandoned,
};

bool OriginIsCryptoTokenExtension(const url::Origin& origin) {
  auto cryptotoken_origin = url::Origin::Create(GURL(kCryptotokenOrigin));
  return cryptotoken_origin == origin;
}

// Ensure that the origin's effective domain is a valid domain.
// Only the domain format of host is valid.
// Reference https://url.spec.whatwg.org/#valid-domain-string and
// https://html.spec.whatwg.org/multipage/origin.html#concept-origin-effective-domain.
bool HasValidEffectiveDomain(url::Origin caller_origin) {
  // For calls originating in the CryptoToken U2F extension, allow CryptoToken
  // to validate domain.
  if (OriginIsCryptoTokenExtension(caller_origin) &&
      base::FeatureList::IsEnabled(device::kWebAuthProxyCryptotoken))
    return true;

  return !caller_origin.opaque() &&
         !url::HostIsIPAddress(caller_origin.host()) &&
         content::IsOriginSecure(caller_origin.GetURL()) &&
         // Additionally, the scheme is required to be HTTP(S). Other schemes
         // may be supported in the future but the webauthn relying party is
         // just the domain of the origin so we would have to define how the
         // authority part of other schemes maps to a "domain" without
         // collisions. Given the |IsOriginSecure| check, just above, HTTP is
         // effectively restricted to just "localhost".
         (caller_origin.scheme() == url::kHttpScheme ||
          caller_origin.scheme() == url::kHttpsScheme);
}

// Ensure the relying party ID is a registrable domain suffix of or equal
// to the origin's effective domain. Reference:
// https://html.spec.whatwg.org/multipage/origin.html#is-a-registrable-domain-suffix-of-or-is-equal-to.
bool IsRelyingPartyIdValid(const std::string& relying_party_id,
                           url::Origin caller_origin) {
  if (OriginIsCryptoTokenExtension(caller_origin) &&
      base::FeatureList::IsEnabled(device::kWebAuthProxyCryptotoken))
    return true;

  if (relying_party_id.empty())
    return false;

  if (caller_origin.host() == relying_party_id)
    return true;

  if (!caller_origin.DomainIs(relying_party_id))
    return false;
  if (!net::registry_controlled_domains::HostHasRegistryControlledDomain(
          caller_origin.host(),
          net::registry_controlled_domains::INCLUDE_UNKNOWN_REGISTRIES,
          net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES))
    return false;
  if (!net::registry_controlled_domains::HostHasRegistryControlledDomain(
          relying_party_id,
          net::registry_controlled_domains::INCLUDE_UNKNOWN_REGISTRIES,
          net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES))
    // TODO(crbug.com/803414): Accept corner-case situations like the following
    // origin: "https://login.awesomecompany",
    // relying_party_id: "awesomecompany".
    return false;
  return true;
}

// Validates whether the given origin is authorized to use the provided App
// ID value, mostly according to the rules in
// https://fidoalliance.org/specs/fido-u2f-v1.2-ps-20170411/fido-appid-and-facets-v1.2-ps-20170411.html#determining-if-a-caller-s-facetid-is-authorized-for-an-appid.
//
// Returns the App ID to use for the request, or base::nullopt if the origin
// is not authorized to use the provided value.
base::Optional<std::string> ProcessAppIdExtension(std::string appid,
                                                  const url::Origin& origin) {
  // The CryptoToken U2F extension checks the appid before calling the WebAuthn
  // API so there is no need to validate it here.
  if (OriginIsCryptoTokenExtension(origin) &&
      base::FeatureList::IsEnabled(device::kWebAuthProxyCryptotoken)) {
    if (!GURL(appid).is_valid()) {
      DCHECK(false) << "cryptotoken request did not set a valid App ID";
      return base::nullopt;
    }
    return appid;
  }

  // Step 1: "If the AppID is not an HTTPS URL, and matches the FacetID of the
  // caller, no additional processing is necessary and the operation may
  // proceed."

  // Webauthn is only supported on secure origins and |HasValidEffectiveDomain|
  // has already checked this property of |origin| before this call. Thus this
  // step is moot.
  DCHECK(content::IsOriginSecure(origin.GetURL()));

  // Step 2: "If the AppID is null or empty, the client must set the AppID to be
  // the FacetID of the caller, and the operation may proceed without additional
  // processing."
  if (appid.empty()) {
    // While the U2F spec says to default the App ID to the Facet ID, which is
    // the origin plus a trailing forward slash [1], cryptotoken and Firefox
    // just use the site's Origin without trailing slash. We follow their
    // implementations rather than the spec.
    //
    // [1]https://fidoalliance.org/specs/fido-v2.0-id-20180227/fido-appid-and-facets-v2.0-id-20180227.html#determining-the-facetid-of-a-calling-application
    appid = origin.Serialize();
  }

  // Step 3: "If the caller's FacetID is an https:// Origin sharing the same
  // host as the AppID, (e.g. if an application hosted at
  // https://fido.example.com/myApp set an AppID of
  // https://fido.example.com/myAppId), no additional processing is necessary
  // and the operation may proceed."
  GURL appid_url = GURL(appid);
  if (!appid_url.is_valid() || appid_url.scheme() != url::kHttpsScheme ||
      appid_url.scheme_piece() != origin.scheme()) {
    return base::nullopt;
  }

  // This check is repeated inside |SameDomainOrHost|, just after this. However
  // it's cheap and mirrors the structure of the spec.
  if (appid_url.host_piece() == origin.host()) {
    return appid;
  }

  // At this point we diverge from the specification in order to avoid the
  // complexity of making a network request which isn't believed to be
  // neccessary in practice. See also
  // https://bugzilla.mozilla.org/show_bug.cgi?id=1244959#c8
  if (net::registry_controlled_domains::SameDomainOrHost(
          appid_url, origin,
          net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES)) {
    return appid;
  }

  // As a compatibility hack, sites within google.com are allowed to assert two
  // special-case AppIDs. Firefox also does this:
  // https://groups.google.com/forum/#!msg/mozilla.dev.platform/Uiu3fwnA2xw/201ynAiPAQAJ
  const GURL kGstatic1 =
      GURL("https://www.gstatic.com/securitykey/origins.json");
  const GURL kGstatic2 =
      GURL("https://www.gstatic.com/securitykey/a/google.com/origins.json");
  DCHECK(kGstatic1.is_valid() && kGstatic2.is_valid());

  if (origin.DomainIs("google.com") && !appid_url.has_ref() &&
      (appid_url.EqualsIgnoringRef(kGstatic1) ||
       appid_url.EqualsIgnoringRef(kGstatic2))) {
    return appid;
  }

  return base::nullopt;
}

device::CtapMakeCredentialRequest CreateCtapMakeCredentialRequest(
    const std::string& client_data_json,
    const blink::mojom::PublicKeyCredentialCreationOptionsPtr& options,
    bool is_incognito) {
  auto credential_params = mojo::ConvertTo<
      std::vector<device::PublicKeyCredentialParams::CredentialInfo>>(
      options->public_key_parameters);

  device::CtapMakeCredentialRequest make_credential_param(
      client_data_json,
      mojo::ConvertTo<device::PublicKeyCredentialRpEntity>(
          options->relying_party),
      mojo::ConvertTo<device::PublicKeyCredentialUserEntity>(options->user),
      device::PublicKeyCredentialParams(std::move(credential_params)));

  auto exclude_list =
      mojo::ConvertTo<std::vector<device::PublicKeyCredentialDescriptor>>(
          options->exclude_credentials);

  make_credential_param.SetExcludeList(std::move(exclude_list));
  make_credential_param.SetHmacSecret(options->hmac_create_secret);
  make_credential_param.set_is_incognito_mode(is_incognito);
  return make_credential_param;
}

device::CtapGetAssertionRequest CreateCtapGetAssertionRequest(
    const std::string& client_data_json,
    const blink::mojom::PublicKeyCredentialRequestOptionsPtr& options,
    base::Optional<std::string> app_id,
    bool is_incognito) {
  device::CtapGetAssertionRequest request_parameter(options->relying_party_id,
                                                    client_data_json);

  auto allowed_list =
      mojo::ConvertTo<std::vector<device::PublicKeyCredentialDescriptor>>(
          options->allow_credentials);

  request_parameter.SetAllowList(std::move(allowed_list));
  request_parameter.SetUserVerification(
      mojo::ConvertTo<device::UserVerificationRequirement>(
          options->user_verification));

  if (app_id) {
    request_parameter.SetAppId(std::move(*app_id));
  }

  if (!options->cable_authentication_data.empty()) {
    request_parameter.SetCableExtension(
        mojo::ConvertTo<std::vector<device::CableDiscoveryData>>(
            options->cable_authentication_data));
  }
  request_parameter.set_is_incognito_mode(is_incognito);
  return request_parameter;
}

// The application parameter is the SHA-256 hash of the UTF-8 encoding of
// the application identity (i.e. relying_party_id) of the application
// requesting the registration.
std::array<uint8_t, crypto::kSHA256Length> CreateApplicationParameter(
    const std::string& relying_party_id) {
  std::array<uint8_t, crypto::kSHA256Length> application_parameter;
  crypto::SHA256HashString(relying_party_id, application_parameter.data(),
                           application_parameter.size());
  return application_parameter;
}

// Parses the FIDO transport types extension from the DER-encoded, X.509
// certificate in |der_cert| and appends any unique transport types found to
// |out_transports|.
void AppendUniqueTransportsFromCertificate(
    base::span<const uint8_t> der_cert,
    std::vector<device::FidoTransportProtocol>* out_transports) {
  // See
  // https://fidoalliance.org/specs/fido-u2f-v1.2-ps-20170411/fido-u2f-authenticator-transports-extension-v1.2-ps-20170411.html#fido-u2f-certificate-transports-extension
  static constexpr uint8_t kTransportTypesOID[] = {
      0x2b, 0x06, 0x01, 0x04, 0x01, 0x82, 0xe5, 0x1c, 0x02, 0x01, 0x01};
  bool present, critical;
  base::StringPiece contents;
  if (!net::asn1::ExtractExtensionFromDERCert(
          base::StringPiece(reinterpret_cast<const char*>(der_cert.data()),
                            der_cert.size()),
          base::StringPiece(reinterpret_cast<const char*>(kTransportTypesOID),
                            sizeof(kTransportTypesOID)),
          &present, &critical, &contents) ||
      !present) {
    return;
  }

  const net::der::Input contents_der(contents);
  net::der::Parser contents_parser(contents_der);
  net::der::BitString transport_bits;
  if (!contents_parser.ReadBitString(&transport_bits)) {
    return;
  }

  // The certificate extension contains a BIT STRING where different bits
  // indicate support for different transports. The following array maps
  // between these bit indexes and the FidoTransportProtocol enum.
  static constexpr struct {
    uint8_t bit_index;
    device::FidoTransportProtocol transport;
  } kTransportMapping[] = {
      // Bit 0 is "Bluetooth Classic", not BLE. Since webauthn doesn't define a
      // transport type for this we ignore it.
      {1, device::FidoTransportProtocol::kBluetoothLowEnergy},
      {2, device::FidoTransportProtocol::kUsbHumanInterfaceDevice},
      {3, device::FidoTransportProtocol::kNearFieldCommunication},
      {4, device::FidoTransportProtocol::kInternal},
  };

  for (const auto& mapping : kTransportMapping) {
    if (transport_bits.AssertsBit(mapping.bit_index) &&
        !base::ContainsValue(*out_transports, mapping.transport)) {
      out_transports->push_back(mapping.transport);
    }
  }
}

enum class AttestationErasureOption {
  kIncludeAttestation,
  kEraseAttestationButIncludeAaguid,
  kEraseAttestationAndAaguid,
};

blink::mojom::MakeCredentialAuthenticatorResponsePtr
CreateMakeCredentialResponse(
    const std::string& client_data_json,
    device::AuthenticatorMakeCredentialResponse response_data,
    AttestationErasureOption attestation_erasure) {
  auto response = blink::mojom::MakeCredentialAuthenticatorResponse::New();
  auto common_info = blink::mojom::CommonCredentialInfo::New();
  common_info->client_data_json.assign(client_data_json.begin(),
                                       client_data_json.end());
  common_info->raw_id = response_data.raw_credential_id();
  common_info->id = response_data.GetId();
  response->info = std::move(common_info);

  // The transport list must not contain duplicates but the order doesn't matter
  // because Blink will sort the resulting strings before returning them.
  std::vector<device::FidoTransportProtocol> transports;
  if (response_data.transport_used()) {
    transports.push_back(*response_data.transport_used());
  }
  // If the attestation certificate specifies that the token supports any other
  // transports, include them in the list.
  base::Optional<base::span<const uint8_t>> leaf_cert =
      response_data.attestation_object()
          .attestation_statement()
          .GetLeafCertificate();
  if (leaf_cert) {
    AppendUniqueTransportsFromCertificate(*leaf_cert, &transports);
  }

  for (auto transport : transports) {
    response->transports.push_back(
        mojo::ConvertTo<blink::mojom::AuthenticatorTransport>(transport));
  }

  const base::Optional<cbor::Value>& maybe_extensions =
      response_data.attestation_object().authenticator_data().extensions();
  if (maybe_extensions) {
    DCHECK(maybe_extensions->is_map());
    const cbor::Value::MapValue& extensions = maybe_extensions->GetMap();
    const auto hmac_secret_it =
        extensions.find(cbor::Value(device::kExtensionHmacSecret));
    if (hmac_secret_it != extensions.end() &&
        hmac_secret_it->second.is_bool()) {
      response->echo_hmac_create_secret = true;
      response->hmac_create_secret = hmac_secret_it->second.GetBool();
    }
  }

  switch (attestation_erasure) {
    case AttestationErasureOption::kIncludeAttestation:
      break;
    case AttestationErasureOption::kEraseAttestationButIncludeAaguid:
      response_data.EraseAttestationStatement(
          device::AttestationObject::AAGUID::kInclude);
      break;
    case AttestationErasureOption::kEraseAttestationAndAaguid:
      response_data.EraseAttestationStatement(
          device::AttestationObject::AAGUID::kErase);
      break;
  }
  response->attestation_object =
      response_data.GetCBOREncodedAttestationObject();

  return response;
}

blink::mojom::GetAssertionAuthenticatorResponsePtr CreateGetAssertionResponse(
    const std::string& client_data_json,
    device::AuthenticatorGetAssertionResponse response_data,
    base::Optional<bool> echo_appid_extension) {
  auto response = blink::mojom::GetAssertionAuthenticatorResponse::New();
  auto common_info = blink::mojom::CommonCredentialInfo::New();
  common_info->client_data_json.assign(client_data_json.begin(),
                                       client_data_json.end());
  common_info->raw_id = response_data.raw_credential_id();
  common_info->id = response_data.GetId();
  response->info = std::move(common_info);
  response->authenticator_data =
      response_data.auth_data().SerializeToByteArray();
  response->signature = response_data.signature();
  if (echo_appid_extension) {
    response->echo_appid_extension = true;
    response->appid_extension = *echo_appid_extension;
  }
  response_data.user_entity()
      ? response->user_handle.emplace(response_data.user_entity()->user_id())
      : response->user_handle.emplace();
  return response;
}

std::string Base64UrlEncode(const base::span<const uint8_t> input) {
  std::string ret;
  base::Base64UrlEncode(
      base::StringPiece(reinterpret_cast<const char*>(input.data()),
                        input.size()),
      base::Base64UrlEncodePolicy::OMIT_PADDING, &ret);
  return ret;
}

base::flat_set<device::FidoTransportProtocol> GetTransportsEnabledByFlags() {
  base::flat_set<device::FidoTransportProtocol> transports;
  transports.insert(device::FidoTransportProtocol::kUsbHumanInterfaceDevice);
  transports.insert(device::FidoTransportProtocol::kInternal);

  // TODO(crbug.com/885165): We should not directly access the BLE stack here.
  // It is used by //device/fido, so its availability should be checked there.
  if (!device::BluetoothAdapterFactory::Get().IsLowEnergySupported())
    return transports;

  if (base::FeatureList::IsEnabled(features::kWebAuthBle)) {
    transports.insert(device::FidoTransportProtocol::kBluetoothLowEnergy);
  }

  // caBLE is independent of the BLE transport.
  if (base::FeatureList::IsEnabled(features::kWebAuthCable)) {
    transports.insert(
        device::FidoTransportProtocol::kCloudAssistedBluetoothLowEnergy);
  }

  return transports;
}

}  // namespace

AuthenticatorImpl::AuthenticatorImpl(RenderFrameHost* render_frame_host)
    : AuthenticatorImpl(render_frame_host,
                        nullptr /* connector */,
                        std::make_unique<base::OneShotTimer>()) {}

AuthenticatorImpl::AuthenticatorImpl(RenderFrameHost* render_frame_host,
                                     service_manager::Connector* connector,
                                     std::unique_ptr<base::OneShotTimer> timer)
    : WebContentsObserver(WebContents::FromRenderFrameHost(render_frame_host)),
      render_frame_host_(render_frame_host),
      connector_(connector),
      transports_(GetTransportsEnabledByFlags()),
      timer_(std::move(timer)),
      binding_(this),
      weak_factory_(this) {
  DCHECK(render_frame_host_);
  DCHECK(timer_);
}

AuthenticatorImpl::~AuthenticatorImpl() {
  // This call exists to assert that |render_frame_host_| outlives this object.
  // If this is violated, ASAN should notice.
  render_frame_host_->GetRoutingID();
}

void AuthenticatorImpl::Bind(blink::mojom::AuthenticatorRequest request) {
  // If |render_frame_host_| is being unloaded then binding requests are
  // rejected.
  if (!render_frame_host_->IsCurrent()) {
    return;
  }

  DCHECK(!binding_.is_bound());
  binding_.Bind(std::move(request));
}

void AuthenticatorImpl::UpdateRequestDelegate() {
  DCHECK(!request_delegate_);
  request_delegate_ =
      GetContentClient()->browser()->GetWebAuthenticationRequestDelegate(
          render_frame_host_);
}

bool AuthenticatorImpl::IsFocused() const {
  return render_frame_host_->IsCurrent() && request_delegate_->IsFocused();
}

// static
std::string AuthenticatorImpl::SerializeCollectedClientDataToJson(
    const std::string& type,
    const std::string& origin,
    base::span<const uint8_t> challenge,
    bool use_legacy_u2f_type_key /* = false */) {
  static constexpr char kChallengeKey[] = "challenge";
  static constexpr char kOriginKey[] = "origin";

  base::DictionaryValue client_data;
  client_data.SetKey(use_legacy_u2f_type_key ? "typ" : "type",
                     base::Value(type));
  client_data.SetKey(kChallengeKey, base::Value(Base64UrlEncode(challenge)));
  client_data.SetKey(kOriginKey, base::Value(origin));

  if (base::RandDouble() < 0.2) {
    // An extra key is sometimes added to ensure that RPs do not make
    // unreasonably specific assumptions about the clientData JSON. This is
    // done in the fashion of
    // https://tools.ietf.org/html/draft-davidben-tls-grease-01
    client_data.SetKey("new_keys_may_be_added_here",
                       base::Value("do not compare clientDataJSON against a "
                                   "template. See https://goo.gl/yabPex"));
  }

  std::string json;
  base::JSONWriter::Write(client_data, &json);
  return json;
}

// mojom::Authenticator
void AuthenticatorImpl::MakeCredential(
    blink::mojom::PublicKeyCredentialCreationOptionsPtr options,
    MakeCredentialCallback callback) {
  if (request_) {
    if (OriginIsCryptoTokenExtension(
            render_frame_host_->GetLastCommittedOrigin())) {
      // Requests originating from cryptotoken will generally outlive any
      // navigation events on the tab of the request's sender. Evict pending
      // requests if cryptotoken sends a new one such that requests from before
      // a navigation event do not prevent new requests. See
      // https://crbug.com/935480.
      Cancel();
    } else {
      std::move(callback).Run(
          blink::mojom::AuthenticatorStatus::PENDING_REQUEST, nullptr);
      return;
    }
  }
  DCHECK(!request_);

  UpdateRequestDelegate();
  if (!request_delegate_) {
    InvokeCallbackAndCleanup(std::move(callback),
                             blink::mojom::AuthenticatorStatus::PENDING_REQUEST,
                             nullptr, Focus::kDontCheck);
    return;
  }

  if (!IsFocused()) {
    InvokeCallbackAndCleanup(std::move(callback),
                             blink::mojom::AuthenticatorStatus::NOT_FOCUSED,
                             nullptr, Focus::kDontCheck);
    return;
  }

  caller_origin_ = render_frame_host_->GetLastCommittedOrigin();
  relying_party_id_ = options->relying_party->id;

  if (!HasValidEffectiveDomain(caller_origin_)) {
    bad_message::ReceivedBadMessage(render_frame_host_->GetProcess(),
                                    bad_message::AUTH_INVALID_EFFECTIVE_DOMAIN);
    InvokeCallbackAndCleanup(std::move(callback),
                             blink::mojom::AuthenticatorStatus::INVALID_DOMAIN,
                             nullptr, Focus::kDontCheck);
    return;
  }

  if (!IsRelyingPartyIdValid(relying_party_id_, caller_origin_)) {
    bad_message::ReceivedBadMessage(render_frame_host_->GetProcess(),
                                    bad_message::AUTH_INVALID_RELYING_PARTY);
    InvokeCallbackAndCleanup(std::move(callback),
                             blink::mojom::AuthenticatorStatus::INVALID_DOMAIN,
                             nullptr, Focus::kDontCheck);
    return;
  }

  if (options->authenticator_selection &&
      options->authenticator_selection->require_resident_key) {
    // Disallow the creation of resident credentials.
    InvokeCallbackAndCleanup(
        std::move(callback),
        blink::mojom::AuthenticatorStatus::RESIDENT_CREDENTIALS_UNSUPPORTED,
        nullptr, Focus::kDontCheck);
    return;
  }

  DCHECK(make_credential_response_callback_.is_null());
  make_credential_response_callback_ = std::move(callback);

  timer_->Start(
      FROM_HERE, options->adjusted_timeout,
      base::BindOnce(&AuthenticatorImpl::OnTimeout, base::Unretained(this)));
  if (!connector_)
    connector_ = ServiceManagerConnection::GetForProcess()->GetConnector();

  // Save client data to return with the authenticator response.
  // TODO(kpaulhamus): Fetch and add the Channel ID/Token Binding ID public key
  // used to communicate with the origin.
  if (OriginIsCryptoTokenExtension(caller_origin_)) {
    // Cryptotoken requests should be proxied without UI.
    request_delegate_->DisableUI();
    // As Cryptotoken validates the origin, accept the relying party id as the
    // origin from requests originating from Cryptotoken. The origin is provided
    // in Cryptotoken requests as the relying party name, which should be used
    // as part of client data.
    client_data_json_ = SerializeCollectedClientDataToJson(
        client_data::kU2fRegisterType, options->relying_party->name,
        std::move(options->challenge), true /* use_legacy_u2f_type_key */);
  } else {
    client_data_json_ = SerializeCollectedClientDataToJson(
        client_data::kCreateType, caller_origin_.Serialize(),
        std::move(options->challenge));
  }

  // U2F requests proxied from the cryptotoken extension are limited to USB
  // devices.
  const auto transports =
      OriginIsCryptoTokenExtension(caller_origin_)
          ? base::flat_set<device::FidoTransportProtocol>(
                {device::FidoTransportProtocol::kUsbHumanInterfaceDevice})
          : transports_;

  auto authenticator_selection_criteria =
      options->authenticator_selection
          ? mojo::ConvertTo<device::AuthenticatorSelectionCriteria>(
                options->authenticator_selection)
          : device::AuthenticatorSelectionCriteria();

  auto ctap_request = CreateCtapMakeCredentialRequest(
      client_data_json_, options, browser_context()->IsOffTheRecord());
  // On dual protocol CTAP2/U2F devices, force credential creation over U2F.
  ctap_request.set_is_u2f_only(OriginIsCryptoTokenExtension(caller_origin_));

  // Compute the effective attestation conveyance preference and set
  // |attestation_requested_| for showing the attestation consent prompt later.
  auto attestation = mojo::ConvertTo<::device::AttestationConveyancePreference>(
      options->attestation);
  if (attestation == ::device::AttestationConveyancePreference::ENTERPRISE &&
      !request_delegate_->ShouldPermitIndividualAttestation(
          relying_party_id_)) {
    attestation = ::device::AttestationConveyancePreference::DIRECT;
  }
  ctap_request.set_attestation_preference(attestation);
  attestation_requested_ =
      attestation != ::device::AttestationConveyancePreference::NONE;

  request_ = std::make_unique<device::MakeCredentialRequestHandler>(
      connector_, transports, std::move(ctap_request),
      std::move(authenticator_selection_criteria),
      base::BindOnce(&AuthenticatorImpl::OnRegisterResponse,
                     weak_factory_.GetWeakPtr()));

  request_delegate_->RegisterActionCallbacks(
      base::BindOnce(&AuthenticatorImpl::Cancel,
                     weak_factory_.GetWeakPtr()) /* cancel_callback */,
      base::BindRepeating(
          &device::FidoRequestHandlerBase::StartAuthenticatorRequest,
          request_->GetWeakPtr()) /* request_callback */,
      base::BindRepeating(
          &device::FidoRequestHandlerBase::PowerOnBluetoothAdapter,
          request_->GetWeakPtr()) /* bluetooth_adapter_power_on_callback */,
      base::BindRepeating(
          &device::FidoRequestHandlerBase::InitiatePairingWithDevice,
          request_->GetWeakPtr()) /* ble_pairing_callback */);
  request_->set_observer(request_delegate_.get());

  request_->SetPlatformAuthenticatorOrMarkUnavailable(
      CreatePlatformAuthenticatorIfAvailable());
}

// mojom:Authenticator
void AuthenticatorImpl::GetAssertion(
    blink::mojom::PublicKeyCredentialRequestOptionsPtr options,
    GetAssertionCallback callback) {
  if (request_) {
    if (OriginIsCryptoTokenExtension(
            render_frame_host_->GetLastCommittedOrigin())) {
      // Requests originating from cryptotoken will generally outlive any
      // navigation events on the tab of the request's sender. Evict pending
      // requests if cryptotoken sends a new one such that requests from before
      // a navigation event do not prevent new requests. See
      // https://crbug.com/935480.
      Cancel();
    } else {
      std::move(callback).Run(
          blink::mojom::AuthenticatorStatus::PENDING_REQUEST, nullptr);
      return;
    }
  }
  DCHECK(!request_);

  UpdateRequestDelegate();
  if (!request_delegate_) {
    InvokeCallbackAndCleanup(std::move(callback),
                             blink::mojom::AuthenticatorStatus::PENDING_REQUEST,
                             nullptr);
    return;
  }

  caller_origin_ = render_frame_host_->GetLastCommittedOrigin();

  // Save client data to return with the authenticator response.
  // TODO(kpaulhamus): Fetch and add the Channel ID/Token Binding ID public key
  // used to communicate with the origin.
  if (OriginIsCryptoTokenExtension(caller_origin_)) {
    request_delegate_->DisableUI();

    // As Cryptotoken validates the origin, accept the relying party id as the
    // origin from requests originating from Cryptotoken.
    client_data_json_ = SerializeCollectedClientDataToJson(
        client_data::kU2fSignType, options->relying_party_id,
        std::move(options->challenge), true /* use_legacy_u2f_type_key */);
  } else {
    client_data_json_ = SerializeCollectedClientDataToJson(
        client_data::kGetType, caller_origin_.Serialize(),
        std::move(options->challenge));
  }

  if (!HasValidEffectiveDomain(caller_origin_)) {
    bad_message::ReceivedBadMessage(render_frame_host_->GetProcess(),
                                    bad_message::AUTH_INVALID_EFFECTIVE_DOMAIN);
    InvokeCallbackAndCleanup(std::move(callback),
                             blink::mojom::AuthenticatorStatus::INVALID_DOMAIN,
                             nullptr);
    return;
  }

  if (!IsRelyingPartyIdValid(options->relying_party_id, caller_origin_)) {
    bad_message::ReceivedBadMessage(render_frame_host_->GetProcess(),
                                    bad_message::AUTH_INVALID_RELYING_PARTY);
    InvokeCallbackAndCleanup(std::move(callback),
                             blink::mojom::AuthenticatorStatus::INVALID_DOMAIN,
                             nullptr);
    return;
  }

  if (options->allow_credentials.empty()) {
    // Chrome currently does not support any resident keys.
    InvokeCallbackAndCleanup(
        std::move(callback),
        blink::mojom::AuthenticatorStatus::RESIDENT_CREDENTIALS_UNSUPPORTED,
        nullptr);
    return;
  }

  if (options->appid) {
    app_id_ = ProcessAppIdExtension(*options->appid, caller_origin_);
    if (!app_id_) {
      std::move(callback).Run(blink::mojom::AuthenticatorStatus::INVALID_DOMAIN,
                              nullptr);
      return;
    }
  }

  // U2F requests proxied from the cryptotoken extension are limited to USB
  // devices.
  const auto transports =
      OriginIsCryptoTokenExtension(caller_origin_)
          ? base::flat_set<device::FidoTransportProtocol>(
                {device::FidoTransportProtocol::kUsbHumanInterfaceDevice})
          : transports_;

  DCHECK(get_assertion_response_callback_.is_null());
  get_assertion_response_callback_ = std::move(callback);

  timer_->Start(
      FROM_HERE, options->adjusted_timeout,
      base::BindOnce(&AuthenticatorImpl::OnTimeout, base::Unretained(this)));

  if (!connector_)
    connector_ = ServiceManagerConnection::GetForProcess()->GetConnector();

  auto ctap_request = CreateCtapGetAssertionRequest(
      client_data_json_, std::move(options), app_id_,
      browser_context()->IsOffTheRecord());
  auto opt_platform_authenticator_info =
      CreatePlatformAuthenticatorIfAvailableAndCheckIfCredentialExists(
          ctap_request);
  request_ = std::make_unique<device::GetAssertionRequestHandler>(
      connector_, transports, std::move(ctap_request),
      base::BindOnce(&AuthenticatorImpl::OnSignResponse,
                     weak_factory_.GetWeakPtr()));

  request_delegate_->RegisterActionCallbacks(
      base::BindOnce(&AuthenticatorImpl::Cancel,
                     weak_factory_.GetWeakPtr()) /* cancel_callback */,
      base::BindRepeating(
          &device::FidoRequestHandlerBase::StartAuthenticatorRequest,
          request_->GetWeakPtr()) /* request_callback */,
      base::BindRepeating(
          &device::FidoRequestHandlerBase::PowerOnBluetoothAdapter,
          request_->GetWeakPtr()) /* bluetooth_adapter_power_on_callback */,
      base::BindRepeating(
          &device::FidoRequestHandlerBase::InitiatePairingWithDevice,
          request_->GetWeakPtr()) /* ble_pairing_callback*/);
  request_->set_observer(request_delegate_.get());

  request_->SetPlatformAuthenticatorOrMarkUnavailable(
      std::move(opt_platform_authenticator_info));
}

void AuthenticatorImpl::IsUserVerifyingPlatformAuthenticatorAvailable(
    IsUserVerifyingPlatformAuthenticatorAvailableCallback callback) {
  const bool result = IsUserVerifyingPlatformAuthenticatorAvailableImpl();
  base::SequencedTaskRunnerHandle::Get()->PostTask(
      FROM_HERE, base::BindOnce(std::move(callback), result));
}

bool AuthenticatorImpl::IsUserVerifyingPlatformAuthenticatorAvailableImpl() {
  //  N.B. request_delegate_ may be nullptr at this point.
  // All platform authenticators are disabled in incognito mode.
  // TODO(martinkr): Revisit incognito handling (crbug/908622).
  if (browser_context()->IsOffTheRecord())
    return false;

#if defined(OS_MACOSX)
  // Touch ID is disabled, regardless of hardware support, if the embedder
  // doesn't support it.
  if (!GetContentClient()
           ->browser()
           ->IsWebAuthenticationTouchIdAuthenticatorSupported())
    return false;

  return device::fido::mac::TouchIdAuthenticator::IsAvailable();
#elif defined(OS_WIN)
  return base::FeatureList::IsEnabled(device::kWebAuthUseNativeWinApi) &&
         device::WinWebAuthnApiAuthenticator::
             IsUserVerifyingPlatformAuthenticatorAvailable();
#else
  return false;
#endif
}

void AuthenticatorImpl::DidFinishNavigation(
    NavigationHandle* navigation_handle) {
  // If the RenderFrameHost itself is navigated then this function will cause
  // request state to be cleaned up. It's also possible for a navigation in the
  // same frame to use a fresh RenderFrameHost. In this case,
  // |render_frame_host_->IsCurrent()| will start returning false, causing all
  // focus checks to fail if any Mojo requests are made in that state.
  if (!navigation_handle->HasCommitted() ||
      navigation_handle->IsSameDocument() ||
      navigation_handle->GetRenderFrameHost() != render_frame_host_) {
    return;
  }

  binding_.Close();
  Cleanup();
}

// Callback to handle the async registration response from a U2fDevice.
void AuthenticatorImpl::OnRegisterResponse(
    device::FidoReturnCode status_code,
    base::Optional<device::AuthenticatorMakeCredentialResponse> response_data,
    base::Optional<device::FidoTransportProtocol> transport_used) {
  if (!request_) {
    // Either the callback was called immediately and |request_| has not yet
    // been assigned (this is a bug), or a navigation caused the request to be
    // canceled while a callback was enqueued.
    return;
  }

  switch (status_code) {
    case device::FidoReturnCode::kUserConsentButCredentialExcluded:
      // Duplicate registration: the new credential would be created on an
      // authenticator that already contains one of the credentials in
      // |exclude_credentials|.
      SignalFailureToRequestDelegate(
          AuthenticatorRequestClientDelegate::InterestingFailureReason::
              kKeyAlreadyRegistered);
      return;
    case device::FidoReturnCode::kAuthenticatorRemovedDuringPINEntry:
      [[fallthrough]];
    case device::FidoReturnCode::kAuthenticatorResponseInvalid:
      // The response from the authenticator was corrupted.
      InvokeCallbackAndCleanup(
          std::move(make_credential_response_callback_),
          blink::mojom::AuthenticatorStatus::NOT_ALLOWED_ERROR, nullptr,
          Focus::kDoCheck);
      return;
    case device::FidoReturnCode::kUserConsentButCredentialNotRecognized:
      // TODO(crbug/876109): This isn't strictly unreachable.
      NOTREACHED();
      return;
    case device::FidoReturnCode::kUserConsentDenied:
      InvokeCallbackAndCleanup(
          std::move(make_credential_response_callback_),
          blink::mojom::AuthenticatorStatus::NOT_ALLOWED_ERROR, nullptr,
          Focus::kDoCheck);
      return;
    case device::FidoReturnCode::kSoftPINBlock:
      SignalFailureToRequestDelegate(
          AuthenticatorRequestClientDelegate::InterestingFailureReason::
              kSoftPINBlock);
      return;
    case device::FidoReturnCode::kHardPINBlock:
      SignalFailureToRequestDelegate(
          AuthenticatorRequestClientDelegate::InterestingFailureReason::
              kHardPINBlock);
      return;
    case device::FidoReturnCode::kSuccess:
      DCHECK(response_data.has_value());

      if (transport_used) {
        request_delegate_->UpdateLastTransportUsed(*transport_used);
      }

      if (attestation_requested_) {
        // Cryptotoken requests may bypass the attestation prompt because the
        // extension implements its own. Invoking the attestation prompt code
        // here would not work anyway, because the WebContents associated with
        // the extension is not associated with any tab and therefore cannot
        // draw modal dialogs for the UI.
        //
        // Note that for AttestationConveyancePreference::NONE, attestation
        // erasure is still performed as usual.
        if (OriginIsCryptoTokenExtension(caller_origin_)) {
          InvokeCallbackAndCleanup(
              std::move(make_credential_response_callback_),
              blink::mojom::AuthenticatorStatus::SUCCESS,
              CreateMakeCredentialResponse(
                  std::move(client_data_json_), std::move(*response_data),
                  AttestationErasureOption::kIncludeAttestation),
              Focus::kDoCheck);
          return;
        }

        UMA_HISTOGRAM_ENUMERATION("WebAuthentication.AttestationPromptResult",
                                  AttestationPromptResult::kQueried);
        awaiting_attestation_response_ = true;
        request_delegate_->ShouldReturnAttestation(
            relying_party_id_,
            base::BindOnce(
                &AuthenticatorImpl::OnRegisterResponseAttestationDecided,
                weak_factory_.GetWeakPtr(), std::move(*response_data)));
        return;
      }

      AttestationErasureOption attestation_erasure =
          AttestationErasureOption::kEraseAttestationAndAaguid;
      if (response_data->IsSelfAttestation()) {
        attestation_erasure = AttestationErasureOption::kIncludeAttestation;
      } else if (transport_used &&
                 *transport_used == device::FidoTransportProtocol::kInternal) {
        // Contrary to what the WebAuthn spec says, for internal (platform)
        // authenticators we do not erase the AAGUID from authenticatorData,
        // even if requested attestationConveyancePreference is "none".
        attestation_erasure =
            AttestationErasureOption::kEraseAttestationButIncludeAaguid;
      }

      InvokeCallbackAndCleanup(
          std::move(make_credential_response_callback_),
          blink::mojom::AuthenticatorStatus::SUCCESS,
          CreateMakeCredentialResponse(std::move(client_data_json_),
                                       std::move(*response_data),
                                       attestation_erasure),
          Focus::kDoCheck);
      return;
  }
  NOTREACHED();
}

void AuthenticatorImpl::OnRegisterResponseAttestationDecided(
    device::AuthenticatorMakeCredentialResponse response_data,
    bool attestation_permitted) {
  awaiting_attestation_response_ = false;
  if (!request_) {
    // The request has already been cleaned up, probably because a navigation
    // occured while the permissions prompt was pending.
    return;
  }

  DCHECK(attestation_requested_);

  if (!attestation_permitted) {
    UMA_HISTOGRAM_ENUMERATION("WebAuthentication.AttestationPromptResult",
                              AttestationPromptResult::kBlocked);
    InvokeCallbackAndCleanup(
        std::move(make_credential_response_callback_),
        blink::mojom::AuthenticatorStatus::NOT_ALLOWED_ERROR, nullptr,
        Focus::kDoCheck);
    return;
  }

  UMA_HISTOGRAM_ENUMERATION("WebAuthentication.AttestationPromptResult",
                            AttestationPromptResult::kAllowed);
  AttestationErasureOption attestation_erasure =
      AttestationErasureOption::kIncludeAttestation;

  // The check for IsAttestationCertificateInappropriatelyIdentifying is
  // performed after the permissions prompt, even though we know the answer
  // before, because this still effectively discloses the make & model of
  // the authenticator: If an RP sees a "none" attestation from Chrome after
  // requesting direct attestation then it knows that it was one of the
  // tokens with inappropriate certs.
  if (response_data.IsAttestationCertificateInappropriatelyIdentifying() &&
      !request_delegate_->ShouldPermitIndividualAttestation(
          relying_party_id_)) {
    // The attestation response is incorrectly individually identifiable, but
    // the consent is for make & model information about a token, not for
    // individually-identifiable information. Erase the attestation to stop it
    // begin a tracking signal.

    // The only way to get the underlying attestation will be to list the RP ID
    // in the enterprise policy, because that enables the individual attestation
    // bit in the register request and permits individual attestation generally.
    attestation_erasure = AttestationErasureOption::kEraseAttestationAndAaguid;
  }

  InvokeCallbackAndCleanup(std::move(make_credential_response_callback_),
                           blink::mojom::AuthenticatorStatus::SUCCESS,
                           CreateMakeCredentialResponse(
                               std::move(client_data_json_),
                               std::move(response_data), attestation_erasure),
                           Focus::kDoCheck);
}

void AuthenticatorImpl::OnSignResponse(
    device::FidoReturnCode status_code,
    base::Optional<device::AuthenticatorGetAssertionResponse> response_data,
    base::Optional<device::FidoTransportProtocol> transport_used) {
  if (!request_) {
    // Either the callback was called immediately and |request_| has not yet
    // been assigned (this is a bug), or a navigation caused the request to be
    // canceled while a callback was enqueued.
    return;
  }

  switch (status_code) {
    case device::FidoReturnCode::kUserConsentButCredentialNotRecognized:
      SignalFailureToRequestDelegate(
          AuthenticatorRequestClientDelegate::InterestingFailureReason::
              kKeyNotRegistered);
      return;
    case device::FidoReturnCode::kAuthenticatorRemovedDuringPINEntry:
      [[fallthrough]];
    case device::FidoReturnCode::kAuthenticatorResponseInvalid:
      // The response from the authenticator was corrupted.
      InvokeCallbackAndCleanup(
          std::move(get_assertion_response_callback_),
          blink::mojom::AuthenticatorStatus::NOT_ALLOWED_ERROR, nullptr);
      return;
    case device::FidoReturnCode::kUserConsentButCredentialExcluded:
      // TODO(crbug/876109): This isn't strictly unreachable.
      NOTREACHED();
      return;
    case device::FidoReturnCode::kUserConsentDenied:
      InvokeCallbackAndCleanup(
          std::move(get_assertion_response_callback_),
          blink::mojom::AuthenticatorStatus::NOT_ALLOWED_ERROR, nullptr);
      return;
    case device::FidoReturnCode::kSoftPINBlock:
      SignalFailureToRequestDelegate(
          AuthenticatorRequestClientDelegate::InterestingFailureReason::
              kSoftPINBlock);
      return;
    case device::FidoReturnCode::kHardPINBlock:
      SignalFailureToRequestDelegate(
          AuthenticatorRequestClientDelegate::InterestingFailureReason::
              kHardPINBlock);
      return;
    case device::FidoReturnCode::kSuccess:
      DCHECK(response_data.has_value());
      if (transport_used) {
        request_delegate_->UpdateLastTransportUsed(*transport_used);
      }

      base::Optional<bool> echo_appid_extension;
      if (app_id_) {
        echo_appid_extension = (response_data->GetRpIdHash() ==
                                CreateApplicationParameter(*app_id_));
      }
      InvokeCallbackAndCleanup(
          std::move(get_assertion_response_callback_),
          blink::mojom::AuthenticatorStatus::SUCCESS,
          CreateGetAssertionResponse(std::move(client_data_json_),
                                     std::move(*response_data),
                                     echo_appid_extension));
      return;
  }
  NOTREACHED();
}

void AuthenticatorImpl::SignalFailureToRequestDelegate(
    AuthenticatorRequestClientDelegate::InterestingFailureReason reason) {
  blink::mojom::AuthenticatorStatus status =
      blink::mojom::AuthenticatorStatus::NOT_ALLOWED_ERROR;

  switch (reason) {
    case AuthenticatorRequestClientDelegate::InterestingFailureReason::
        kKeyAlreadyRegistered:
      status = blink::mojom::AuthenticatorStatus::CREDENTIAL_EXCLUDED;
      break;
    case AuthenticatorRequestClientDelegate::InterestingFailureReason::
        kKeyNotRegistered:
      status = blink::mojom::AuthenticatorStatus::CREDENTIAL_NOT_RECOGNIZED;
      break;
    case AuthenticatorRequestClientDelegate::InterestingFailureReason::kTimeout:
      status = blink::mojom::AuthenticatorStatus::NOT_ALLOWED_ERROR;
      break;
    case AuthenticatorRequestClientDelegate::InterestingFailureReason::
        kSoftPINBlock:
      status = blink::mojom::AuthenticatorStatus::NOT_ALLOWED_ERROR;
      break;
    case AuthenticatorRequestClientDelegate::InterestingFailureReason::
        kHardPINBlock:
      status = blink::mojom::AuthenticatorStatus::NOT_ALLOWED_ERROR;
  }

  error_awaiting_user_acknowledgement_ = status;

  // If WebAuthnUi is enabled, this error blocks until after receiving user
  // acknowledgement. Otherwise, the error is returned right away.
  if (request_delegate_->DoesBlockRequestOnFailure(reason)) {
    // Cancel pending authenticator requests before the error dialog is shown.
    request_->CancelActiveAuthenticators();
    return;
  }

  FailWithErrorAndCleanup();
}  // namespace content

void AuthenticatorImpl::FailWithErrorAndCleanup() {
  DCHECK(make_credential_response_callback_ ||
         get_assertion_response_callback_);
  if (make_credential_response_callback_) {
    InvokeCallbackAndCleanup(std::move(make_credential_response_callback_),
                             error_awaiting_user_acknowledgement_, nullptr,
                             Focus::kDontCheck);
  } else if (get_assertion_response_callback_) {
    InvokeCallbackAndCleanup(std::move(get_assertion_response_callback_),
                             error_awaiting_user_acknowledgement_, nullptr);
  }
}

// TODO(crbug.com/814418): Add web tests to verify timeouts are
// indistinguishable from NOT_ALLOWED_ERROR cases.
void AuthenticatorImpl::OnTimeout() {
  DCHECK(request_delegate_);
  if (awaiting_attestation_response_) {
    UMA_HISTOGRAM_ENUMERATION("WebAuthentication.AttestationPromptResult",
                              AttestationPromptResult::kTimeout);
    awaiting_attestation_response_ = false;
  }

  SignalFailureToRequestDelegate(
      AuthenticatorRequestClientDelegate::InterestingFailureReason::kTimeout);
}

void AuthenticatorImpl::Cancel() {
  // If response callback is invoked already, then ignore cancel request.
  if (!make_credential_response_callback_ && !get_assertion_response_callback_)
    return;

  FailWithErrorAndCleanup();
}

void AuthenticatorImpl::InvokeCallbackAndCleanup(
    MakeCredentialCallback callback,
    blink::mojom::AuthenticatorStatus status,
    blink::mojom::MakeCredentialAuthenticatorResponsePtr response,
    Focus check_focus) {
  if (check_focus != Focus::kDontCheck && !(request_delegate_ && IsFocused())) {
    std::move(callback).Run(blink::mojom::AuthenticatorStatus::NOT_FOCUSED,
                            nullptr);
  } else {
    std::move(callback).Run(status, std::move(response));
  }

  Cleanup();
}

void AuthenticatorImpl::InvokeCallbackAndCleanup(
    GetAssertionCallback callback,
    blink::mojom::AuthenticatorStatus status,
    blink::mojom::GetAssertionAuthenticatorResponsePtr response) {
  std::move(callback).Run(status, std::move(response));
  Cleanup();
}

void AuthenticatorImpl::Cleanup() {
  if (awaiting_attestation_response_) {
    UMA_HISTOGRAM_ENUMERATION("WebAuthentication.AttestationPromptResult",
                              AttestationPromptResult::kAbandoned);
    awaiting_attestation_response_ = false;
  }

  timer_->Stop();
  request_.reset();
  request_delegate_.reset();
  make_credential_response_callback_.Reset();
  get_assertion_response_callback_.Reset();
  client_data_json_.clear();
  app_id_.reset();
  attestation_requested_ = false;
  error_awaiting_user_acknowledgement_ =
      blink::mojom::AuthenticatorStatus::NOT_ALLOWED_ERROR;
}

BrowserContext* AuthenticatorImpl::browser_context() const {
  return content::WebContents::FromRenderFrameHost(render_frame_host_)
      ->GetBrowserContext();
}

#if defined(OS_MACOSX)
namespace {
std::unique_ptr<device::fido::mac::TouchIdAuthenticator>
CreateTouchIdAuthenticatorIfAvailable(
    const AuthenticatorRequestClientDelegate* request_delegate) {
  // Not all embedders may provide an authenticator config.
  auto opt_authenticator_config =
      request_delegate->GetTouchIdAuthenticatorConfig();
  if (!opt_authenticator_config) {
    return nullptr;
  }
  return device::fido::mac::TouchIdAuthenticator::CreateIfAvailable(
      std::move(opt_authenticator_config->keychain_access_group),
      std::move(opt_authenticator_config->metadata_secret));
}
}  // namespace
#endif

base::Optional<device::PlatformAuthenticatorInfo>
AuthenticatorImpl::CreatePlatformAuthenticatorIfAvailable() {
  // Incognito mode disables platform authenticators, so check for availability
  // first.
  if (!IsUserVerifyingPlatformAuthenticatorAvailableImpl()) {
    return base::nullopt;
  }
#if defined(OS_MACOSX)
  return device::PlatformAuthenticatorInfo(
      CreateTouchIdAuthenticatorIfAvailable(request_delegate_.get()), false);
#else
  return base::nullopt;
#endif
}

base::Optional<device::PlatformAuthenticatorInfo> AuthenticatorImpl::
    CreatePlatformAuthenticatorIfAvailableAndCheckIfCredentialExists(
        const device::CtapGetAssertionRequest& request) {
  // Incognito mode disables platform authenticators, so check for availability
  // first.
  if (!IsUserVerifyingPlatformAuthenticatorAvailableImpl()) {
    return base::nullopt;
  }
#if defined(OS_MACOSX)
  std::unique_ptr<device::fido::mac::TouchIdAuthenticator> authenticator =
      CreateTouchIdAuthenticatorIfAvailable(request_delegate_.get());
  const bool has_credential =
      authenticator->HasCredentialForGetAssertionRequest(request);
  return device::PlatformAuthenticatorInfo(std::move(authenticator),
                                           has_credential);
#else
  return base::nullopt;
#endif
}

}  // namespace content
