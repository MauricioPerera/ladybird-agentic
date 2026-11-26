/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CredentialsContainer, https://w3c.github.io/webappsec-credential-management/#credentialscontainer

use core::ffi::c_void;

use libjs_rust::api::{Trace, Value, Vm};

use super::credential::{AnyCredential, CredentialMediationRequirement};
use super::federated_credential::{FederatedCredentialInit, FederatedCredentialRequestOptions};
use super::password_credential::PasswordCredentialInit;
use crate::bindings::interface::{Ref, Wrappable, allocate, cell_into_abi};
use crate::bindings::promise::{WebIdlPromise, reject_promise, resolve_promise};
use crate::bindings::{CallContext, WebIdlResult};
use crate::dom::abort_signal::AbortSignal;

/// https://w3c.github.io/webappsec-credential-management/#dictdef-credentialrequestoptions
pub(crate) struct CredentialRequestOptions {
    #[expect(dead_code, reason = "get() does not read federated options yet")]
    pub(crate) federated: Option<FederatedCredentialRequestOptions>,
    #[expect(dead_code, reason = "get() does not read mediation yet")]
    pub(crate) mediation: CredentialMediationRequirement,
    #[expect(dead_code, reason = "get() does not read password yet")]
    pub(crate) password: bool,
    pub(crate) signal: Option<Ref<AbortSignal>>,
}

/// https://w3c.github.io/webappsec-credential-management/#dictdef-credentialcreationoptions
pub(crate) struct CredentialCreationOptions {
    #[expect(dead_code, reason = "create() does not read federated options yet")]
    pub(crate) federated: Option<FederatedCredentialInit>,
    #[expect(dead_code, reason = "create() does not read mediation yet")]
    pub(crate) mediation: CredentialMediationRequirement,
    #[expect(dead_code, reason = "create() does not read password yet")]
    pub(crate) password: Option<PasswordCredentialInit>,
    pub(crate) signal: Option<Ref<AbortSignal>>,
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CredentialsContainer;

impl Wrappable for CredentialsContainer {}

impl CredentialsContainer {
    // https://w3c.github.io/webappsec-credential-management/#dom-credentialscontainer-get
    pub(crate) fn get(
        &self,
        cx: &mut CallContext,
        promise: WebIdlPromise,
        options: CredentialRequestOptions,
    ) -> WebIdlResult<()> {
        if let Some(signal) = options.signal.as_ref()
            && signal.aborted()
        {
            reject_promise(cx.vm(), promise, signal.reason());
            return Ok(());
        }
        resolve_promise(cx.vm(), promise, Value::NULL);
        Ok(())
    }

    // https://w3c.github.io/webappsec-credential-management/#dom-credentialscontainer-store
    pub(crate) fn store(&self, cx: &mut CallContext, promise: WebIdlPromise, _: AnyCredential) -> WebIdlResult<()> {
        resolve_promise(cx.vm(), promise, Value::UNDEFINED);
        Ok(())
    }

    // https://w3c.github.io/webappsec-credential-management/#dom-credentialscontainer-create
    pub(crate) fn create(
        &self,
        cx: &mut CallContext,
        promise: WebIdlPromise,
        options: CredentialCreationOptions,
    ) -> WebIdlResult<()> {
        if let Some(signal) = options.signal.as_ref()
            && signal.aborted()
        {
            reject_promise(cx.vm(), promise, signal.reason());
            return Ok(());
        }
        resolve_promise(cx.vm(), promise, Value::NULL);
        Ok(())
    }

    // https://w3c.github.io/webappsec-credential-management/#dom-credentialscontainer-preventsilentaccess
    pub(crate) fn prevent_silent_access(&self, cx: &mut CallContext, promise: WebIdlPromise) -> WebIdlResult<()> {
        resolve_promise(cx.vm(), promise, Value::UNDEFINED);
        Ok(())
    }
}

/// # Safety
///
/// The VM must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_credentials_container_create(vm: *const c_void) -> *mut c_void {
    // SAFETY: The caller passes a live VM.
    let vm = unsafe { &*vm.cast::<Vm>() };
    cell_into_abi(allocate(vm, |_| CredentialsContainer))
}
