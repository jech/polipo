/*
Copyright (c) 2003 by Juliusz Chroboczek

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#define CONFIG_INT 0
#define CONFIG_OCTAL 1
#define CONFIG_TIME 2
#define CONFIG_BOOLEAN 3
#define CONFIG_TRISTATE 4
#define CONFIG_TETRASTATE 5
#define CONFIG_PENTASTATE 6
#define CONFIG_FLOAT 7
#define CONFIG_ATOM 8
#define CONFIG_ATOM_LOWER 9
#define CONFIG_INT_LIST 10
#define CONFIG_ATOM_LIST 11
#define CONFIG_ATOM_LIST_LOWER 12

typedef struct _ConfigVariable {
    AtomPtr name;
    int type;
    union {
        int *i;
        float *f;
        struct _Atom **a;
        struct _AtomList **al;
        struct _IntList **il;
    } value;
    char *help;
    struct _ConfigVariable *next;
} ConfigVariableRec, *ConfigVariablePtr;

#define CONFIG_VARIABLE(name, type, help) \
    declareConfigVariable(internAtom(#name), type, &name, help)

void declareConfigVariable(AtomPtr name, int type, void *value, char *help);
void printConfigVariables(FILE *out, int html);
int parseConfigLine(char *line, char *filename, int lineno);
int parseConfigFile(AtomPtr);
