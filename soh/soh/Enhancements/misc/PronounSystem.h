#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// CVar name — integer, 0=He/Him/His (default/no-op), 1=She/Her/Her, 2=They/Them/Their
#define CVAR_PRONOUN_MODE "gEnhancements.PronounMode"

// Called immediately after the message buffer is populated in z_message_PAL.c.
// Scans buf[0..(*length)-1] for gendered pronouns and replaces them in-place
// according to the current CVAR_PRONOUN_MODE value.
// *length is updated if any replacements change the buffer size.
// No-op when mode == 0 or when not connected (always safe to call).
void PronounSystem_FilterMessageBuffer(char* buf, int* length);

#ifdef __cplusplus
}
#endif
