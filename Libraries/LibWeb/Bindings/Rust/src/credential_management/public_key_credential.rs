/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! PublicKeyCredential, https://w3c.github.io/webauthn/#iface-pkcredential

use ak::Utf16String;
use libjs_rust::api::{Trace, Value};

use super::credential::Credential;
use crate::bindings::interface::{Inherits, Wrappable};
use crate::bindings::promise::{WebIdlPromise, create_resolved_promise};
use crate::bindings::{CallContext, WebIdlResult};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct PublicKeyCredential {
    credential: Credential,
    authenticator_attachment: Option<Utf16String>,
}

impl Wrappable for PublicKeyCredential {}

impl Inherits<Credential> for PublicKeyCredential {
    fn parent(&self) -> &Credential {
        &self.credential
    }
}

impl PublicKeyCredential {
    // https://w3c.github.io/webauthn/#dom-publickeycredential-authenticatorattachment
    pub(crate) fn authenticator_attachment(&self, _: &mut CallContext) -> WebIdlResult<Option<Utf16String>> {
        Ok(self.authenticator_attachment.clone())
    }

    // https://w3c.github.io/webauthn/#dom-publickeycredential-isuserverifyingplatformauthenticatoravailable
    pub(crate) fn is_user_verifying_platform_authenticator_available(cx: &mut CallContext) -> WebIdlResult<WebIdlPromise> {
        Ok(create_resolved_promise(cx.vm(), cx.realm(), Value::FALSE))
    }

    // https://w3c.github.io/webauthn/#dom-publickeycredential-isconditionalmediationavailable
    pub(crate) fn is_conditional_mediation_available(cx: &mut CallContext) -> WebIdlResult<WebIdlPromise> {
        Ok(create_resolved_promise(cx.vm(), cx.realm(), Value::FALSE))
    }
}
