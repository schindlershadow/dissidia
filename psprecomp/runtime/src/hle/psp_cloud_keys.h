// OAuth client of this project's Google Cloud app ("TVs and Limited Input devices" type, scope drive.appdata only).
// For this client type Google does not treat the secret as confidential (it ships in every installed app); it identifies the
// project for quota and consent, and cannot access anyone's Drive without that person's own sign-in. A fork should register
// its own client (docs/cloud-sync.md) and replace these two values, or set PSPRECOMP_GOOGLE_CLIENT_ID / _SECRET.
#pragma once

#define PSP_GOOGLE_CLIENT_ID "56854558926-kfbg391eldnbmehuocps99gj29vmhbqq.apps.googleusercontent.com"
#define PSP_GOOGLE_CLIENT_SECRET "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"
