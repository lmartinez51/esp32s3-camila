#ifndef ASSISTANTS_H
#define ASSISTANTS_H

#include <stdbool.h>

typedef struct
{
    char *user;
    char *threadId;
} ThreadEntry;

typedef struct
{
    const char *apiKey;
    const char *santiagoId;
    ThreadEntry threads[5]; // Hasta 5 threads asociados a usuarios
} Assistants;

char *assistants_assistantManager(Assistants *assistants, const char *user, const char *message);
char *createThread(Assistants *assistants, const char *context);
char *createMessage(Assistants *assistants, const char *threadId, const char *content);
bool runAssistant(Assistants *assistants, const char *userThreadId, const char *assistantId);
char *listMessages(Assistants *assistants, const char *threadId);

#endif // ASSISTANTS_H
