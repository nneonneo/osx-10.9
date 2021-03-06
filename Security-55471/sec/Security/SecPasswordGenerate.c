/*
 * Copyright (c) 2013 Apple Inc. All Rights Reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */

/*
 * SecPasswordStrength.c
 */

#include <limits.h>
#include <CoreFoundation/CoreFoundation.h>
#include <Security/SecItem.h>
#include <Security/SecBase.h>
#include <Security/SecRandom.h>
#include "SecPasswordGenerate.h"
#include <AssertMacros.h>
#include <fcntl.h>
#include <unistd.h>
#include <utilities/SecCFWrappers.h>
#include <utilities/SecCFRelease.h>
#include <utilities/SecCFError.h>

// Keys for external dictionaries with password generation requirements we read from plist.
CFStringRef kSecPasswordMinLengthKey = CFSTR("PasswordMinLength");
CFStringRef kSecPasswordMaxLengthKey = CFSTR("PasswordMaxLength");
CFStringRef kSecPasswordAllowedCharactersKey = CFSTR("PasswordAllowedCharacters");
CFStringRef kSecPasswordRequiredCharactersKey = CFSTR("PasswordRequiredCharacters");
CFStringRef kSecPasswordDefaultForType = CFSTR("PasswordDefaultForType");

CFStringRef kSecPasswordDisallowedCharacters = CFSTR("PasswordDisallowedCharacters");
CFStringRef kSecPasswordCantStartWithChars = CFSTR("PasswordCantStartWithChars");
CFStringRef kSecPasswordCantEndWithChars = CFSTR("PasswordCantEndWithChars");
CFStringRef kSecPasswordContainsNoMoreThanNSpecificCharacters = CFSTR("PasswordContainsNoMoreThanNSpecificCharacters");
CFStringRef kSecPasswordContainsAtLeastNSpecificCharacters = CFSTR("PasswordContainsAtLeastNSpecificCharacters");
CFStringRef kSecPasswordContainsNoMoreThanNConsecutiveIdenticalCharacters = CFSTR("PasswordContainsNoMoreThanNConsecutiveIdenticalCharacters");
CFStringRef kSecPasswordCharacterCount = CFSTR("PasswordCharacterCount");
CFStringRef kSecPasswordCharacters = CFSTR("PasswordCharacters");

CFStringRef kSecPasswordGroupSize = CFSTR("PasswordGroupSize");
CFStringRef kSecPasswordNumberOfGroups = CFSTR("PasswordNumberOfGroups");
CFStringRef kSecPasswordSeparator = CFSTR("SecPasswordSeparator");

// Keys for internally used dictionaries with password generation parameters (never exposed to external API).
static CFStringRef kSecUseDefaultPasswordFormatKey = CFSTR("UseDefaultPasswordFormat");
static CFStringRef kSecNumberOfRequiredRandomCharactersKey = CFSTR("NumberOfRequiredRandomCharacters");
static CFStringRef kSecAllowedCharactersKey = CFSTR("AllowedCharacters");
static CFStringRef kSecRequiredCharacterSetsKey = CFSTR("RequiredCharacterSets");

static CFIndex defaultNumberOfRandomCharacters = 20;
static CFIndex defaultPINLength = 4;
static CFIndex defaultiCloudPasswordLength = 24;
static CFIndex defaultWifiPasswordLength = 12;

static CFStringRef defaultWifiCharacters = CFSTR("abcdefghijklmnopqrstuvwxyz1234567890");
static CFStringRef defaultPINCharacters = CFSTR("0123456789");
static CFStringRef defaultiCloudCharacters = CFSTR("ABCDEFGHJKLMNPQRSTUVWXYZ23456789");
static CFStringRef defaultCharacters = CFSTR("abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ123456789");

static CFCharacterSetRef uppercaseLetterCharacterSet;
static CFCharacterSetRef lowercaseLetterCharacterSet;
static CFCharacterSetRef decimalDigitCharacterSet;
static CFCharacterSetRef punctuationCharacterSet;

static CFIndex alphabetSetSize = 26;
static CFIndex decimalSetSize = 10;
static CFIndex punctuationSetSize = 33;
static double entropyStrengthThreshold = 35.0;

/*
 generated with ruby badpins.rb | gperf
 See this for PIN list:
 A birthday present every eleven wallets? The security of customer-chosen banking PINs (2012),  by Joseph Bonneau , Sören Preibusch , Ross Anderson
 */
const char *in_word_set (const char *str, unsigned int len);

#if !((' ' == 32) && ('!' == 33) && ('"' == 34) && ('#' == 35) \
&& ('%' == 37) && ('&' == 38) && ('\'' == 39) && ('(' == 40) \
&& (')' == 41) && ('*' == 42) && ('+' == 43) && (',' == 44) \
&& ('-' == 45) && ('.' == 46) && ('/' == 47) && ('0' == 48) \
&& ('1' == 49) && ('2' == 50) && ('3' == 51) && ('4' == 52) \
&& ('5' == 53) && ('6' == 54) && ('7' == 55) && ('8' == 56) \
&& ('9' == 57) && (':' == 58) && (';' == 59) && ('<' == 60) \
&& ('=' == 61) && ('>' == 62) && ('?' == 63) && ('A' == 65) \
&& ('B' == 66) && ('C' == 67) && ('D' == 68) && ('E' == 69) \
&& ('F' == 70) && ('G' == 71) && ('H' == 72) && ('I' == 73) \
&& ('J' == 74) && ('K' == 75) && ('L' == 76) && ('M' == 77) \
&& ('N' == 78) && ('O' == 79) && ('P' == 80) && ('Q' == 81) \
&& ('R' == 82) && ('S' == 83) && ('T' == 84) && ('U' == 85) \
&& ('V' == 86) && ('W' == 87) && ('X' == 88) && ('Y' == 89) \
&& ('Z' == 90) && ('[' == 91) && ('\\' == 92) && (']' == 93) \
&& ('^' == 94) && ('_' == 95) && ('a' == 97) && ('b' == 98) \
&& ('c' == 99) && ('d' == 100) && ('e' == 101) && ('f' == 102) \
&& ('g' == 103) && ('h' == 104) && ('i' == 105) && ('j' == 106) \
&& ('k' == 107) && ('l' == 108) && ('m' == 109) && ('n' == 110) \
&& ('o' == 111) && ('p' == 112) && ('q' == 113) && ('r' == 114) \
&& ('s' == 115) && ('t' == 116) && ('u' == 117) && ('v' == 118) \
&& ('w' == 119) && ('x' == 120) && ('y' == 121) && ('z' == 122) \
&& ('{' == 123) && ('|' == 124) && ('}' == 125) && ('~' == 126))
/* The character set is not based on ISO-646.  */
error "gperf generated tables don't work with this execution character set. Please report a bug to <bug-gnu-gperf@gnu.org>."
#endif


#define TOTAL_KEYWORDS 100
#define MIN_WORD_LENGTH 4
#define MAX_WORD_LENGTH 4
#define MIN_HASH_VALUE 21
#define MAX_HASH_VALUE 275
/* maximum key range = 255, duplicates = 0 */

#ifdef __GNUC__
__inline
#else
#ifdef __cplusplus
inline
#endif
#endif
static unsigned int pinhash (const char *str, unsigned int len)
{
    static unsigned short asso_values[] =
    {
        276, 276, 276, 276, 276, 276, 276, 276, 276, 276,
        276, 276, 276, 276, 276, 276, 276, 276, 276, 276,
        276, 276, 276, 276, 276, 276, 276, 276, 276, 276,
        276, 276, 276, 276, 276, 276, 276, 276, 276, 276,
        276, 276, 276, 276, 276, 276, 276, 276,   5,   0,
        10,  10,  30,  50, 100, 120,  70,  25,  57,  85,
        2,   4,   1,  19,  14,  11,  92, 276, 276, 276,
        276, 276, 276, 276, 276, 276, 276, 276, 276, 276,
        276, 276, 276, 276, 276, 276, 276, 276, 276, 276,
        276, 276, 276, 276, 276, 276, 276, 276, 276, 276,
        276, 276, 276, 276, 276, 276, 276, 276, 276, 276,
        276, 276, 276, 276, 276, 276, 276, 276, 276, 276,
        276, 276, 276, 276, 276, 276, 276, 276, 276, 276,
        276, 276, 276, 276, 276, 276, 276, 276, 276, 276,
        276, 276, 276, 276, 276, 276, 276, 276, 276, 276,
        276, 276, 276, 276, 276, 276, 276, 276, 276, 276,
        276, 276, 276, 276, 276, 276, 276, 276, 276, 276,
        276, 276, 276, 276, 276, 276, 276, 276, 276, 276,
        276, 276, 276, 276, 276, 276, 276, 276, 276, 276,
        276, 276, 276, 276, 276, 276, 276, 276, 276, 276,
        276, 276, 276, 276, 276, 276, 276, 276, 276, 276,
        276, 276, 276, 276, 276, 276, 276, 276, 276, 276,
        276, 276, 276, 276, 276, 276, 276, 276, 276, 276,
        276, 276, 276, 276, 276, 276, 276, 276, 276, 276,
        276, 276, 276, 276, 276, 276, 276, 276, 276, 276,
        276, 276, 276, 276, 276, 276, 276, 276, 276, 276,
        276, 276, 276, 276, 276
    };
    return len + asso_values[(unsigned char)str[3]+9] + asso_values[(unsigned char)str[2]] + asso_values[(unsigned char)str[1]] + asso_values[(unsigned char)str[0]+3];
}

//pins that reached the top 20 list
static const char *blacklist[] = {"1234", "1004", "2000", "1122", "4321", "2001", "2580"};

bool SecPasswordIsPasswordWeak(CFStringRef passcode)
{
    uppercaseLetterCharacterSet = CFCharacterSetGetPredefined(kCFCharacterSetUppercaseLetter);
    lowercaseLetterCharacterSet = CFCharacterSetGetPredefined(kCFCharacterSetLowercaseLetter);
    decimalDigitCharacterSet = CFCharacterSetGetPredefined(kCFCharacterSetDecimalDigit);
    punctuationCharacterSet = CFCharacterSetGetPredefined(kCFCharacterSetPunctuation);
    
    bool isNumber = true;
    char* pin = NULL;
    
    //length checks
    if( CFStringGetLength(passcode) < 4 ){
        return true; //weak password
    }
    //check to see if passcode is a number
    for(CFIndex i = 0; i < CFStringGetLength(passcode); i++){
        if( CFStringFindCharacterFromSet(passcode, decimalDigitCharacterSet, CFRangeMake(i,1), 0, NULL))
            continue;
        else {
            isNumber = false;
            break;
        }
    }
    //checking to see if it's a 4 digit pin
    if(isNumber && CFStringGetLength(passcode) == 4){
        
        pin = CFStringToCString(passcode);
        if(in_word_set(pin, 4)){
            free(pin);
            return true;
        }

        CFIndex blacklistLength = (CFIndex)sizeof(blacklist)/sizeof(blacklist[0]);
        
        //not all the same number
        if(pin[0] == pin[1] == pin[2] == pin[3]){
            free(pin);
            return true; //weak password
        }
        //first two digits being the same and the last two digits being the same
        if ( pin[0] == pin[1] && pin[2] == pin[3]){
            free(pin);
            return true; //weak password
        }
        //first two digits not being the same as the last two digits
        if(pin[0] == pin[2] && pin[1] == pin[3]){
            free(pin);
            return true; //weak password
        }
        //not in this list
        for(CFIndex i = 0; i < blacklistLength; i++)
        {
            const char* blackCode = blacklist[i];
            if(0 == strcmp(blackCode, pin))
            {
                free(pin);
                return true; //weak password
            }
        }
    }
    else if(isNumber){ //dealing with a numeric PIN
        pin = CFStringToCString(passcode);
        //check if PIN is all the same number
        for(int i = 0; i < CFStringGetLength(passcode); i++){
            if(i+1 >= CFStringGetLength(passcode)){
                free(pin);
                return true;
            }
            else if (pin[i] == pin[i+1])
                continue;
            else
                break;
        }
        //check if PIN is a bunch of incrementing numbers
        for(int i = 0; i < CFStringGetLength(passcode); i++){
            if(i == CFStringGetLength(passcode)-1){
                free(pin);
                return true;
            }
            else if ((pin[i] + 1) == pin[i+1])
                continue;
            else
                break;
        }
        //check if PIN is a bunch of decrementing numbers
        for(int i = 0; i < CFStringGetLength(passcode); i++){
            if(i == CFStringGetLength(passcode)-1){
                free(pin);
                return true;
            }
            else if ((pin[i]) == (pin[i+1] +1))
                continue;
            else
                break;
        }
    }
    else{ // password is complex, evaluate entropy
        int u = 0;
        int l = 0;
        int d = 0;
        int p = 0;
        int characterSet = 0;
        
        //calculate new entropy
        for(CFIndex i = 0; i < CFStringGetLength(passcode); i++){
            
            if( CFStringFindCharacterFromSet(passcode, uppercaseLetterCharacterSet, CFRangeMake(i,1), kCFCompareBackwards, NULL)){
                u++;
                continue;
            }
            if( CFStringFindCharacterFromSet(passcode, lowercaseLetterCharacterSet, CFRangeMake(i,1), kCFCompareBackwards, NULL)){
                l++;
                continue;
            }
            if( CFStringFindCharacterFromSet(passcode, decimalDigitCharacterSet, CFRangeMake(i,1), kCFCompareBackwards, NULL)){
                d++;
                continue;
            }
            if( CFStringFindCharacterFromSet(passcode, punctuationCharacterSet, CFRangeMake(i,1), kCFCompareBackwards, NULL)){
                p++;
                continue;
            }
            
        }
        if(u > 0){
            characterSet += alphabetSetSize;
        }
        if(l > 0){
            characterSet += alphabetSetSize;
        }
        if(d > 0){
            characterSet += decimalSetSize;
        }
        if(p > 0){
            characterSet += punctuationSetSize;
        }
        
        double strength = CFStringGetLength(passcode)*log2(characterSet);
        
        if(strength < entropyStrengthThreshold){
            return true; //weak
        }
        else
            return false; //strong
    }
    if(pin)
        free(pin);
    
    return false; //strong password
    
}

static void getUniformRandomNumbers(uint8_t* buffer, size_t numberOfDesiredNumbers, uint8_t upperBound)
{
    
    // The values returned by SecRandomCopyBytes are uniformly distributed in the range [0, 255]. If we try to map
    // these values onto a smaller range using modulo we will introduce a bias towards lower numbers in situations
    // where our smaller range doesn’t evenly divide in to [0, 255]. For example, with the desired range of [0, 54]
    // the ranges 0..54, 55..109, 110..164, and 165..219 are uniformly distributed, but the range 220..255 modulo 55
    // is only distributed over [0, 35], giving significant bias to these lower values. So, we ignore random numbers
    // that would introduce this bias.
    uint8_t limitAvoidingModuloBias = UCHAR_MAX - (UCHAR_MAX % upperBound);
    
    for (size_t numberOfAcceptedNumbers = 0; numberOfAcceptedNumbers < numberOfDesiredNumbers; ) {
        if (SecRandomCopyBytes(kSecRandomDefault, numberOfDesiredNumbers - numberOfAcceptedNumbers, buffer + numberOfAcceptedNumbers) == -1)
            continue;
        for (size_t i = numberOfAcceptedNumbers; i < numberOfDesiredNumbers; ++i) {
            if (buffer[i] < limitAvoidingModuloBias)
                buffer[numberOfAcceptedNumbers++] = buffer[i] % upperBound;
        }
    }
}

static bool passwordContainsRequiredCharacters(CFStringRef password, CFArrayRef requiredCharacterSets)
{
    CFCharacterSetRef characterSet;
    
    for (CFIndex i = 0; i< CFArrayGetCount(requiredCharacterSets); i++) {
        characterSet = CFArrayGetValueAtIndex(requiredCharacterSets, i);
        CFRange rangeToSearch = CFRangeMake(0, CFStringGetLength(password));
        require_quiet(CFStringFindCharacterFromSet(password, characterSet, rangeToSearch, 0, NULL), fail);
    }
    return true;

fail:
    return false;
    
}

static bool passwordContainsLessThanNIdenticalCharacters(CFStringRef password, CFIndex identicalCount)
{
    unsigned char Char, nextChar;
    int repeating = 0;
    
    for(CFIndex i = 0; i < CFStringGetLength(password); i++){
        Char = CFStringGetCharacterAtIndex(password, i);
        for(CFIndex j = i; j< CFStringGetLength(password); j++){
            nextChar = CFStringGetCharacterAtIndex(password, j);
            require_quiet(repeating <= identicalCount, fail);
            if(Char == nextChar){
                repeating++;
            }else{
                repeating = 0;
                break;
            }
        }
    }
    return true;
fail:
    return false;
}

static bool passwordContainsAtLeastNCharacters(CFStringRef password, CFStringRef characters, CFIndex N)
{
    CFCharacterSetRef characterSet = NULL;
    characterSet = CFCharacterSetCreateWithCharactersInString(kCFAllocatorDefault, characters);
    CFIndex counter = 0;
    
    for(CFIndex i = 0; i < CFStringGetLength(password); i++){
        if(CFStringFindCharacterFromSet(password, characterSet, CFRangeMake(i, 1), 0, NULL))
            counter++;
    }
    CFReleaseNull(characterSet);
    if(counter < N)
        return false;
    else
        return true;
}

static bool passwordContainsLessThanNCharacters(CFStringRef password, CFStringRef characters, CFIndex N)
{
    CFCharacterSetRef characterSet = NULL;
    characterSet = CFCharacterSetCreateWithCharactersInString(kCFAllocatorDefault, characters);
    CFIndex counter = 0;
    
    for(CFIndex i = 0; i < CFStringGetLength(password); i++){
        if(CFStringFindCharacterFromSet(password, characterSet, CFRangeMake(i, 1), 0, NULL))
            counter++;
    }
    CFReleaseNull(characterSet);
    if(counter > N)
        return false;
    else
        return true;
}

static bool passwordDoesNotContainCharacters(CFStringRef password, CFStringRef prohibitedCharacters)
{
    CFCharacterSetRef characterSet = NULL;
    characterSet = CFCharacterSetCreateWithCharactersInString(kCFAllocatorDefault, prohibitedCharacters);
    CFRange rangeToSearch = CFRangeMake(0, CFStringGetLength(password));
    
    require_quiet(!CFStringFindCharacterFromSet(password, characterSet, rangeToSearch, 0, NULL), fail);
    CFReleaseNull(characterSet);
    return true;
fail:
    CFReleaseNull(characterSet);
    return false;
}

static void getPasswordRandomCharacters(CFStringRef *returned, CFDictionaryRef requirements, CFIndex *numberOfRandomCharacters, CFStringRef allowedCharacters)
{
    uint8_t randomNumbers[*numberOfRandomCharacters];
    unsigned char randomCharacters[*numberOfRandomCharacters];
    getUniformRandomNumbers(randomNumbers, *numberOfRandomCharacters, CFStringGetLength(allowedCharacters));
    
    CFTypeRef prohibitedCharacters = NULL;
    CFStringRef temp = NULL;
    
    if(!CFDictionaryGetValueIfPresent(requirements, kSecPasswordDisallowedCharacters, &prohibitedCharacters))
        prohibitedCharacters = NULL;
    
    //it's faster for long characters to check each character produced for these cases
    for (CFIndex i = 0; i < *numberOfRandomCharacters; ++i){
        //check prohibited characters
        UniChar randomChar[1];
        randomChar[0] = CFStringGetCharacterAtIndex(allowedCharacters, randomNumbers[i]);
        temp = CFStringCreateWithCharacters(kCFAllocatorDefault, randomChar, 1);
        
        if(prohibitedCharacters != NULL)
        {
            if(!passwordDoesNotContainCharacters(temp, prohibitedCharacters)){
                //change up the random numbers so we don't get the same index into allowed
                getUniformRandomNumbers(randomNumbers, *numberOfRandomCharacters, CFStringGetLength(allowedCharacters));
                i--;
                continue;
            }
        }
        randomCharacters[i] = (unsigned char)randomChar[0];
    }
    
    CFReleaseNull(temp);

    *returned = CFStringCreateWithBytes(kCFAllocatorDefault, randomCharacters, *numberOfRandomCharacters, kCFStringEncodingUTF8, false);
}

static bool doesPasswordEndWith(CFStringRef password, CFStringRef prohibitedCharacters)
{
    CFCharacterSetRef characterSet = NULL;
    characterSet = CFCharacterSetCreateWithCharactersInString(kCFAllocatorDefault, prohibitedCharacters);
    
    CFRange rangeToSearch = CFRangeMake(CFStringGetLength(password) - CFStringGetLength(prohibitedCharacters), CFStringGetLength(prohibitedCharacters));
    require_quiet(0 == CFStringCompareWithOptions(password, prohibitedCharacters, rangeToSearch, 0), fail);
    CFReleaseNull(characterSet);
    return false;
fail:
    CFReleaseNull(characterSet);
    return true;
}

static bool doesPasswordStartWith(CFStringRef password, CFStringRef prohibitedCharacters)
{
    CFCharacterSetRef characterSet = NULL;
    characterSet = CFCharacterSetCreateWithCharactersInString(kCFAllocatorDefault, prohibitedCharacters);
    
    CFRange rangeToSearch = CFRangeMake(0, CFStringGetLength(prohibitedCharacters));
    require_quiet(0 == CFStringCompareWithOptions(password, prohibitedCharacters, rangeToSearch, 0), fail);
    CFReleaseNull(characterSet);
    return false; //does not start with prohibitedCharacters
fail:
    CFReleaseNull(characterSet);
    return true;
}

static void passwordGenerateDefaultParametersDictionary(CFDictionaryRef *returned, SecPasswordType type, CFDictionaryRef requirements){
    
    CFMutableArrayRef requiredCharacterSets = NULL;
    CFNumberRef numReqChars = NULL;
    CFStringRef defaultPasswordFormat = NULL;
    requiredCharacterSets = CFArrayCreateMutable(NULL, 0, NULL);
    defaultPasswordFormat = CFSTR("true");
    CFTypeRef groupSizeRef = NULL, numberOfGroupsRef = NULL;
    CFIndex groupSize, numberOfGroups;
    switch(type){
        case(kSecPasswordTypeiCloudRecovery):
            numReqChars = CFNumberCreateWithCFIndex(kCFAllocatorDefault, defaultiCloudPasswordLength);
            groupSize = 4;
            numberOfGroups = 6;
            groupSizeRef = CFNumberCreate(NULL, kCFNumberIntType, &groupSize);
            numberOfGroupsRef = CFNumberCreate(NULL, kCFNumberIntType, &numberOfGroups);
            
            uppercaseLetterCharacterSet = CFCharacterSetGetPredefined(kCFCharacterSetUppercaseLetter);
            decimalDigitCharacterSet = CFCharacterSetGetPredefined(kCFCharacterSetDecimalDigit);
            CFArrayAppendValue(requiredCharacterSets, uppercaseLetterCharacterSet);
            CFArrayAppendValue(requiredCharacterSets, decimalDigitCharacterSet);
            *returned = CFDictionaryCreateForCFTypes(kCFAllocatorDefault,
                                                    kSecUseDefaultPasswordFormatKey,   defaultPasswordFormat,
                                                    kSecNumberOfRequiredRandomCharactersKey, numReqChars,
                                                    kSecAllowedCharactersKey,   defaultiCloudCharacters,
                                                    kSecRequiredCharacterSetsKey, requiredCharacterSets,
                                                    kSecPasswordGroupSize, groupSizeRef,
                                                    kSecPasswordNumberOfGroups, numberOfGroupsRef,
                                                    NULL);
            break;
            
        case(kSecPasswordTypePIN):
            numReqChars = CFNumberCreateWithCFIndex(kCFAllocatorDefault, defaultPINLength);
            groupSize = 4;
            numberOfGroups = 1;
            groupSizeRef = CFNumberCreate(NULL, kCFNumberIntType, &groupSize);
            numberOfGroupsRef = CFNumberCreate(NULL, kCFNumberIntType, &numberOfGroups);

            decimalDigitCharacterSet = CFCharacterSetGetPredefined(kCFCharacterSetDecimalDigit);
            CFArrayAppendValue(requiredCharacterSets, decimalDigitCharacterSet);
            *returned = CFDictionaryCreateForCFTypes(kCFAllocatorDefault,
                                                                         kSecUseDefaultPasswordFormatKey,   defaultPasswordFormat,
                                                                         kSecNumberOfRequiredRandomCharactersKey, numReqChars,
                                                                         kSecAllowedCharactersKey,   defaultPINCharacters,
                                                                         kSecRequiredCharacterSetsKey, requiredCharacterSets,
                                                                         kSecPasswordGroupSize, groupSizeRef,
                                                                         kSecPasswordNumberOfGroups, numberOfGroupsRef,
                                                                         NULL);
            break;

        case(kSecPasswordTypeWifi):
            groupSize = 4;
            numberOfGroups = 3;
            groupSizeRef = CFNumberCreate(NULL, kCFNumberIntType, &groupSize);
            numberOfGroupsRef = CFNumberCreate(NULL, kCFNumberIntType, &numberOfGroups);

            lowercaseLetterCharacterSet = CFCharacterSetGetPredefined(kCFCharacterSetLowercaseLetter);
            decimalDigitCharacterSet = CFCharacterSetGetPredefined(kCFCharacterSetDecimalDigit);

            numReqChars = CFNumberCreateWithCFIndex(kCFAllocatorDefault, defaultWifiPasswordLength);
            CFArrayAppendValue(requiredCharacterSets, lowercaseLetterCharacterSet);
            CFArrayAppendValue(requiredCharacterSets, decimalDigitCharacterSet);
            *returned = CFDictionaryCreateForCFTypes(kCFAllocatorDefault,
                                                                         kSecUseDefaultPasswordFormatKey,   defaultPasswordFormat,
                                                                         kSecNumberOfRequiredRandomCharactersKey, numReqChars,
                                                                         kSecAllowedCharactersKey,   defaultWifiCharacters,
                                                                         kSecRequiredCharacterSetsKey, requiredCharacterSets,
                                                                         kSecPasswordGroupSize, groupSizeRef,
                                                                         kSecPasswordNumberOfGroups, numberOfGroupsRef,
                                                                         NULL);
            break;
            
        default:
            groupSize = 4;
            numberOfGroups = 6;
            groupSizeRef = CFNumberCreate(NULL, kCFNumberIntType, &groupSize);
            numberOfGroupsRef = CFNumberCreate(NULL, kCFNumberIntType, &numberOfGroups);
            uppercaseLetterCharacterSet = CFCharacterSetGetPredefined(kCFCharacterSetUppercaseLetter);
            lowercaseLetterCharacterSet = CFCharacterSetGetPredefined(kCFCharacterSetLowercaseLetter);
            decimalDigitCharacterSet = CFCharacterSetGetPredefined(kCFCharacterSetDecimalDigit);
            CFArrayAppendValue(requiredCharacterSets, uppercaseLetterCharacterSet);
            CFArrayAppendValue(requiredCharacterSets, lowercaseLetterCharacterSet);
            CFArrayAppendValue(requiredCharacterSets, decimalDigitCharacterSet);

            numReqChars = CFNumberCreateWithCFIndex(kCFAllocatorDefault, defaultNumberOfRandomCharacters);
            *returned = CFDictionaryCreateForCFTypes(kCFAllocatorDefault,
                                                                         kSecUseDefaultPasswordFormatKey,   defaultPasswordFormat,
                                                                         kSecNumberOfRequiredRandomCharactersKey, numReqChars,
                                                                         kSecAllowedCharactersKey,   defaultCharacters,
                                                                         kSecRequiredCharacterSetsKey, requiredCharacterSets,
                                                                         kSecPasswordGroupSize, groupSizeRef,
                                                                         kSecPasswordNumberOfGroups, numberOfGroupsRef,
                                                                         NULL);
            
            

            break;
    }
    
    CFReleaseNull(numReqChars);
    CFReleaseNull(requiredCharacterSets);
    CFReleaseNull(groupSizeRef);
    CFReleaseNull(numberOfGroupsRef);
}
static void passwordGenerationParametersDictionary(CFDictionaryRef *returned, SecPasswordType type, CFDictionaryRef requirements)
{
    CFMutableArrayRef requiredCharacterSets = CFArrayCreateMutable(NULL, 0, NULL);
    CFArrayRef requiredCharactersArray = NULL;
    CFNumberRef numReqChars = NULL;
    CFIndex numberOfRequiredRandomCharacters;
    CFStringRef allowedCharacters = NULL, useDefaultPasswordFormat = NULL;
    uint64_t valuePtr;
    CFTypeRef prohibitedCharacters = NULL, endWith = NULL, startWith = NULL,
            groupSizeRef = NULL, numberOfGroupsRef = NULL, separatorRef = NULL,
            atMostCharactersRef = NULL,atLeastCharactersRef = NULL, identicalRef = NULL;
    
    CFNumberRef min = (CFNumberRef)CFDictionaryGetValue(requirements, kSecPasswordMinLengthKey);
    CFNumberRef max = (CFNumberRef)CFDictionaryGetValue(requirements, kSecPasswordMaxLengthKey);
    
    CFNumberGetValue(min, kCFNumberSInt64Type, &valuePtr);
    CFIndex minPasswordLength = (long)valuePtr;
    CFNumberGetValue(max, kCFNumberSInt64Type, &valuePtr);
    CFIndex maxPasswordLength = (long)valuePtr;
    
    // If requirements allow, we will generate the password in default format.
    useDefaultPasswordFormat = CFSTR("true");
    numberOfRequiredRandomCharacters = defaultNumberOfRandomCharacters;
    
    if(type == kSecPasswordTypePIN)
    {
        if( maxPasswordLength && minPasswordLength )
            numberOfRequiredRandomCharacters = maxPasswordLength;
        else if( !maxPasswordLength && minPasswordLength )
            numberOfRequiredRandomCharacters = minPasswordLength;
        else if( !minPasswordLength && maxPasswordLength )
            numberOfRequiredRandomCharacters = maxPasswordLength;
        else
            numberOfRequiredRandomCharacters = defaultPINLength;
        
        allowedCharacters = CFSTR("0123456789");
        CFArrayAppendValue(requiredCharacterSets, decimalDigitCharacterSet);
        requiredCharactersArray = CFArrayCreateCopy(NULL, requiredCharacterSets);
        useDefaultPasswordFormat = CFSTR("false");
    }
    else{
        if (minPasswordLength && minPasswordLength > defaultNumberOfRandomCharacters) {
            useDefaultPasswordFormat = CFSTR("false");
            numberOfRequiredRandomCharacters = minPasswordLength;
        }
        if (maxPasswordLength && maxPasswordLength < defaultNumberOfRandomCharacters) {
            useDefaultPasswordFormat = CFSTR("false");
            numberOfRequiredRandomCharacters = maxPasswordLength;
        }
        if (maxPasswordLength && minPasswordLength && maxPasswordLength == minPasswordLength && maxPasswordLength != defaultNumberOfRandomCharacters){
            useDefaultPasswordFormat = CFSTR("false");
            numberOfRequiredRandomCharacters = maxPasswordLength;
        }
        allowedCharacters = (CFStringRef)CFDictionaryGetValue(requirements, kSecPasswordAllowedCharactersKey);
        requiredCharactersArray = (CFArrayRef)CFDictionaryGetValue(requirements, kSecPasswordRequiredCharactersKey);
    }
    if(!CFDictionaryGetValueIfPresent(requirements, kSecPasswordDisallowedCharacters, &prohibitedCharacters))
        prohibitedCharacters = NULL;
    
    if(!CFDictionaryGetValueIfPresent(requirements, kSecPasswordCantEndWithChars, &endWith))
        endWith = NULL;
    
    if(!CFDictionaryGetValueIfPresent(requirements, kSecPasswordCantStartWithChars, &startWith))
        startWith = NULL;
    
    if(!CFDictionaryGetValueIfPresent(requirements, kSecPasswordGroupSize, &groupSizeRef))
        groupSizeRef = NULL;
    
    if(!CFDictionaryGetValueIfPresent(requirements, kSecPasswordNumberOfGroups, &numberOfGroupsRef))
        numberOfGroupsRef = NULL;
    
    if(!CFDictionaryGetValueIfPresent(requirements, kSecPasswordSeparator, &separatorRef))
        separatorRef = NULL;
    
    if(!CFDictionaryGetValueIfPresent(requirements, kSecPasswordContainsNoMoreThanNSpecificCharacters, &atMostCharactersRef))
        atMostCharactersRef = NULL;
    
    if(!CFDictionaryGetValueIfPresent(requirements, kSecPasswordContainsAtLeastNSpecificCharacters, &atLeastCharactersRef))
        atLeastCharactersRef = NULL;
    
    if(!CFDictionaryGetValueIfPresent(requirements, kSecPasswordContainsNoMoreThanNConsecutiveIdenticalCharacters, &identicalRef))
        identicalRef = NULL;
    
    if (allowedCharacters) {
        if( false == CFStringFindWithOptions(allowedCharacters, CFSTR("-"), CFRangeMake(0, CFStringGetLength(allowedCharacters)), kCFCompareCaseInsensitive, NULL))
            useDefaultPasswordFormat = CFSTR("false");
    } else
        allowedCharacters = defaultCharacters;
    
    // In default password format, we use dashes only as separators, not as symbols you can encounter at a random position.
    if (useDefaultPasswordFormat == CFSTR("false")){
        CFMutableStringRef mutatedAllowedCharacters = CFStringCreateMutableCopy(kCFAllocatorDefault, CFStringGetLength(allowedCharacters), allowedCharacters);
        CFStringFindAndReplace (mutatedAllowedCharacters, CFSTR("-"), CFSTR(""), CFRangeMake(0, CFStringGetLength(allowedCharacters)),kCFCompareCaseInsensitive);
        allowedCharacters = CFStringCreateCopy(kCFAllocatorDefault, mutatedAllowedCharacters);
    }
    
    if (requiredCharactersArray) {
        for (CFIndex i = 0; i < CFArrayGetCount(requiredCharactersArray); i++){
            CFCharacterSetRef stringWithRequiredCharacters = CFArrayGetValueAtIndex(requiredCharactersArray, i);
            if( CFStringFindCharacterFromSet(allowedCharacters, stringWithRequiredCharacters, CFRangeMake(0, CFStringGetLength(allowedCharacters)), 0, NULL)){
                CFArrayAppendValue(requiredCharacterSets, stringWithRequiredCharacters);
            }
        }
    } else{
        uppercaseLetterCharacterSet = CFCharacterSetGetPredefined(kCFCharacterSetUppercaseLetter);
        lowercaseLetterCharacterSet = CFCharacterSetGetPredefined(kCFCharacterSetLowercaseLetter);
        decimalDigitCharacterSet = CFCharacterSetGetPredefined(kCFCharacterSetDecimalDigit);
        CFArrayAppendValue(requiredCharacterSets, uppercaseLetterCharacterSet);
        CFArrayAppendValue(requiredCharacterSets, lowercaseLetterCharacterSet);
        CFArrayAppendValue(requiredCharacterSets, decimalDigitCharacterSet);
    }
    
    
    if (CFArrayGetCount(requiredCharacterSets) > numberOfRequiredRandomCharacters) {
        CFReleaseNull(requiredCharacterSets);
        requiredCharacterSets = NULL;
    }
    //create new CFDictionary
    numReqChars = CFNumberCreateWithCFIndex(kCFAllocatorDefault, numberOfRequiredRandomCharacters);
    CFMutableDictionaryRef updatedConstraints = CFDictionaryCreateMutableForCFTypes(kCFAllocatorDefault);
    CFDictionaryAddValue(updatedConstraints, kSecUseDefaultPasswordFormatKey, useDefaultPasswordFormat);
    CFDictionarySetValue(updatedConstraints, kSecNumberOfRequiredRandomCharactersKey, numReqChars);
    CFDictionaryAddValue(updatedConstraints, kSecAllowedCharactersKey, allowedCharacters);
    if(requiredCharacterSets)
        CFDictionaryAddValue(updatedConstraints, kSecRequiredCharacterSetsKey, requiredCharacterSets);
    
    //add the prohibited characters string if it exists to the new dictionary
    if(prohibitedCharacters)
        CFDictionaryAddValue(updatedConstraints, kSecPasswordDisallowedCharacters, prohibitedCharacters);
    
    //add the characters the password can't end with if it exists to the new dictionary
    if(endWith)
        CFDictionaryAddValue(updatedConstraints, kSecPasswordCantEndWithChars, endWith);
    
    //add the characters the password can't start with if it exists to the new dictionary
    if(startWith)
        CFDictionaryAddValue(updatedConstraints, kSecPasswordCantStartWithChars, startWith);
    
    if(groupSizeRef)
        CFDictionaryAddValue(updatedConstraints, kSecPasswordGroupSize, groupSizeRef);
    
    if(numberOfGroupsRef)
        CFDictionaryAddValue(updatedConstraints, kSecPasswordNumberOfGroups, numberOfGroupsRef);
    
    if(separatorRef)
        CFDictionaryAddValue(updatedConstraints, kSecPasswordSeparator, separatorRef);
    
    if(atMostCharactersRef)
        CFDictionaryAddValue(updatedConstraints, kSecPasswordContainsNoMoreThanNSpecificCharacters, atMostCharactersRef);
    
    if(atLeastCharactersRef)
        CFDictionaryAddValue(updatedConstraints, kSecPasswordContainsAtLeastNSpecificCharacters, atLeastCharactersRef);
    
    if(identicalRef)
        CFDictionaryAddValue(updatedConstraints, kSecPasswordContainsNoMoreThanNConsecutiveIdenticalCharacters, identicalRef);
    
    CFReleaseNull(useDefaultPasswordFormat);
    CFReleaseNull(numReqChars);
    CFReleaseNull(allowedCharacters);
    CFReleaseNull(requiredCharacterSets);
    
    *returned = CFDictionaryCreateCopy(kCFAllocatorDefault, updatedConstraints);
}

static bool isDictionaryFormattedProperly(SecPasswordType type, CFDictionaryRef passwordRequirements, CFErrorRef *error){
    
    CFTypeRef defaults = NULL;
    CFErrorRef tempError = NULL;
    if(passwordRequirements == NULL){
        return true;
    }
    
    if( CFDictionaryGetValueIfPresent(passwordRequirements, kSecPasswordDefaultForType, &defaults) ){
        if(isString(defaults) == true && 0 == CFStringCompare(defaults, CFSTR("true"), 0)){
            return true;
        }
    }
    //only need to check max and min pin length formatting
    if(type == kSecPasswordTypePIN){
        CFTypeRef minTest = NULL, maxTest = NULL;
        uint64_t valuePtr;
        CFIndex minPasswordLength = 0, maxPasswordLength= 0;
        
        if( CFDictionaryGetValueIfPresent(passwordRequirements, kSecPasswordDefaultForType, &defaults) ){
            if(isString(defaults) == true && 0 == CFStringCompare(defaults, CFSTR("true"), 0)){
                return true;
            }
        }
        //check if the values exist!
        if( CFDictionaryGetValueIfPresent(passwordRequirements, kSecPasswordMaxLengthKey, &maxTest) ){
            require_action_quiet(isNull(maxTest)!= true, fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("To generate a password, need a max length"), (CFIndex)errSecBadReq, NULL));
            require_action_quiet(isNumber(maxTest), fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("The password's max length must be a CFNumberRef"), (CFIndex)errSecBadReq, NULL));
            
        }
        if (CFDictionaryGetValueIfPresent(passwordRequirements, kSecPasswordMinLengthKey, &minTest) ){
            require_action_quiet(isNull(minTest)!= true, fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("To generate a password, need a min length"), (CFIndex)errSecBadReq, NULL));
            require_action_quiet(isNumber(minTest), fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("The password's min length must be a CFNumberRef"), (CFIndex)errSecBadReq, NULL));
        }
        //check if the values exist!
        if(maxTest){
            CFNumberRef max = (CFNumberRef)maxTest;
            CFNumberGetValue(max, kCFNumberSInt64Type, &valuePtr);
            maxPasswordLength = (long)valuePtr;
        }
        if(minTest){
            CFNumberRef min = (CFNumberRef)minTest;
            CFNumberGetValue(min, kCFNumberSInt64Type, &valuePtr);
            minPasswordLength = (long)valuePtr;
        }
        //make sure min and max make sense respective to each other and that they aren't less than 4 digits.
        require_action_quiet(minPasswordLength && maxPasswordLength && minPasswordLength <= maxPasswordLength, fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("The password's length parameters make no sense ( is max < min ?)"),  (CFIndex)errSecBadReq, NULL));
        require_action_quiet((minPasswordLength && minPasswordLength >= 4) || (maxPasswordLength && maxPasswordLength >= 4), fail, tempError = CFErrorCreate(kCFAllocatorDefault,  CFSTR("The password's length parameters make no sense ( is max < min ?)"),  (CFIndex)errSecBadReq, NULL));
    }
    else{
        CFTypeRef allowedTest, maxTest, minTest, requiredTest, prohibitedCharacters, endWith, startWith,
        groupSizeRef, numberOfGroupsRef, separatorRef, atMostCharactersRef,
        atLeastCharactersRef, thresholdRef, identicalRef, characters;
        uint64_t valuePtr;
        
        //check if the values exist!
        require_action_quiet(CFDictionaryGetValueIfPresent(passwordRequirements, kSecPasswordAllowedCharactersKey, &allowedTest), fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("Need a string of characters; password must only contain characters in this string"),  (CFIndex)errSecBadReq, NULL));
        require_action_quiet( CFDictionaryGetValueIfPresent(passwordRequirements, kSecPasswordMaxLengthKey, &maxTest), fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("To generate a password, need a max length"), (CFIndex)errSecBadReq, NULL));
        require_action_quiet( CFDictionaryGetValueIfPresent(passwordRequirements, kSecPasswordMinLengthKey, &minTest), fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("To generate a password, need a min length"), (CFIndex)errSecBadReq, NULL));
        require_action_quiet(CFDictionaryGetValueIfPresent(passwordRequirements, kSecPasswordRequiredCharactersKey, &requiredTest), fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("Need an array of character sets, password must have at least 1 character from each set"), (CFIndex)errSecBadReq, NULL));
        
        //check if values are null?
        require_action_quiet(isNull(allowedTest) != true, fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("Need a string of characters; password must only contain characters in this string"),  (CFIndex)errSecBadReq, NULL));
        require_action_quiet(isNull(maxTest)!= true, fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("To generate a password, need a max length"), (CFIndex)errSecBadReq, NULL));
        require_action_quiet(isNull(minTest)!= true, fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("To generate a password, need a min length"), (CFIndex)errSecBadReq, NULL));
        require_action_quiet(isNull(requiredTest)!= true, fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("Need an array of character sets, password must have at least 1 character from each set"), (CFIndex)errSecBadReq, NULL));
        
        //check if the values are correct
        require_action_quiet(isString(allowedTest), fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("The password's allowed characters must be a CFStringRef"), (CFIndex)errSecBadReq, NULL));
        require_action_quiet(isNumber(maxTest), fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("The password's max length must be a CFNumberRef"), (CFIndex)errSecBadReq, NULL));
        require_action_quiet(isNumber(minTest), fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("The password's min length must be a CFNumberRef"), (CFIndex)errSecBadReq, NULL));
        require_action_quiet(isArray(requiredTest), fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("The password's required characters must be an array of CFCharacterSetRefs"), (CFIndex)errSecBadReq, NULL));
        
        CFNumberGetValue(minTest, kCFNumberSInt64Type, &valuePtr);
        CFIndex minPasswordLength = (long)valuePtr;
        CFNumberGetValue(maxTest, kCFNumberSInt64Type, &valuePtr);
        CFIndex maxPasswordLength = (long)valuePtr;
        
        require_action_quiet(minPasswordLength <= maxPasswordLength, fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("The password's length parameters make no sense ( is max < min ?)"),  (CFIndex)errSecBadReq, NULL));

        require_action_quiet(CFStringGetLength((CFStringRef)allowedTest) != 0, fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("Need a string of characters; password must only contain characters in this string"),  (CFIndex)errSecBadReq, NULL));
        require_action_quiet(CFArrayGetCount((CFArrayRef)requiredTest), fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("Need an array of character sets, password must have at least 1 character from each set"), (CFIndex)errSecBadReq, NULL));
        
        if(CFDictionaryGetValueIfPresent(passwordRequirements, kSecPasswordDisallowedCharacters, &prohibitedCharacters)){
            require_action_quiet(isNull(prohibitedCharacters) != true, fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("Disallowed Characters dictionary parameter is either null or not a string"),  (CFIndex)errSecBadReq, NULL));
            require_action_quiet(isString(prohibitedCharacters), fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("Disallowed Characters dictionary parameter is either null or not a string"), (CFIndex)errSecBadReq, NULL));
        }
        if(CFDictionaryGetValueIfPresent(passwordRequirements, kSecPasswordCantEndWithChars, &endWith)){
            require_action_quiet(isNull(endWith) != true, fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("The dictionary parameter 'EndWith' is either null or not a string"),  (CFIndex)errSecBadReq, NULL));
            require_action_quiet(isString(endWith), fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("The dictionary parameter 'EndWith' is either null or not a string"), (CFIndex)errSecBadReq, NULL));
        }
        if(CFDictionaryGetValueIfPresent(passwordRequirements, kSecPasswordCantStartWithChars, &startWith)){
            require_action_quiet(isNull(startWith) != true, fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("The dictionary parameter 'StartWith' is either null or not a string"),  (CFIndex)errSecBadReq, NULL));
            require_action_quiet(isString(startWith), fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("The dictionary parameter 'StartWith' is either null or not a string"), (CFIndex)errSecBadReq, NULL));
        }
        if(CFDictionaryGetValueIfPresent(passwordRequirements, kSecPasswordGroupSize, &groupSizeRef)){
            require_action_quiet(isNull(groupSizeRef) != true, fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("The dictionary parameter 'groupsize' is either null or not a number"),  (CFIndex)errSecBadReq, NULL));
            require_action_quiet(isNumber(groupSizeRef), fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("The dictionary parameter 'groupsize' is either null or not a number"), (CFIndex)errSecBadReq, NULL));
        }
        if(CFDictionaryGetValueIfPresent(passwordRequirements, kSecPasswordNumberOfGroups, &numberOfGroupsRef)){
            require_action_quiet(isNull(numberOfGroupsRef) != true, fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("The dictionary parameter 'number of groupds' is either null or not a number"),  (CFIndex)errSecBadReq, NULL));
            require_action_quiet(isNumber(numberOfGroupsRef), fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("The dictionary parameter 'number of groupds' is either null or not a number"), (CFIndex)errSecBadReq, NULL));
        }
        if(CFDictionaryGetValueIfPresent(passwordRequirements, kSecPasswordSeparator, &separatorRef)){
            require_action_quiet(isNull(separatorRef) != true, fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("The dictionary parameter 'password separator character' is either null or not a string"),  (CFIndex)errSecBadReq, NULL));
            require_action_quiet(isString(separatorRef), fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("The dictionary parameter 'password separator character' is either null or not a string"), (CFIndex)errSecBadReq, NULL));
        }
        
        if(CFDictionaryGetValueIfPresent(passwordRequirements, kSecPasswordContainsNoMoreThanNSpecificCharacters, &atMostCharactersRef)){
            require_action_quiet(isNull(atMostCharactersRef) != true, fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("The dictionary parameter 'At Most N Characters' is either null or not a string"),  (CFIndex)errSecBadReq, NULL));
            require_action_quiet(isDictionary(atMostCharactersRef), fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("The dictionary parameter 'At Most N Characters' is either null or not a string"), (CFIndex)errSecBadReq, NULL));
            
            require_action_quiet(CFDictionaryGetValueIfPresent(atMostCharactersRef, kSecPasswordCharacterCount, &thresholdRef) != false, fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("The dictionary parameter 'At Most N Characters' is either null or not a string"),  (CFIndex)errSecBadReq, NULL));
            require_action_quiet(isNull(thresholdRef) != true, fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("The dictionary parameter 'characters' is either null or not a number"),  (CFIndex)errSecBadReq, NULL));
            require_action_quiet(isNumber(thresholdRef), fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("The dictionary parameter 'characters' is either null or not a number"), (CFIndex)errSecBadReq, NULL));
            
            require_action_quiet(CFDictionaryGetValueIfPresent(atMostCharactersRef, kSecPasswordCharacters, &characters)!= false, fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("The dictionary parameter 'At Most N Characters' is either null or not a string"),  (CFIndex)errSecBadReq, NULL));
            require_action_quiet(isNull(characters) != true, fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("The dictionary parameter 'Characters' is either null or not a string"),  (CFIndex)errSecBadReq, NULL));
            require_action_quiet(isString(characters), fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("The dictionary parameter 'Characters' is either null or not a string"), (CFIndex)errSecBadReq, NULL));
        }
        
        if(CFDictionaryGetValueIfPresent(passwordRequirements, kSecPasswordContainsAtLeastNSpecificCharacters, &atLeastCharactersRef)){
            require_action_quiet(isNull(atLeastCharactersRef) != true, fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("The dictionary parameter 'At Least N Characters' is either null or not a string"),  (CFIndex)errSecBadReq, NULL));
            require_action_quiet(isDictionary(atLeastCharactersRef), fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("The dictionary parameter 'At Least N Characters' is either null or not a string"), (CFIndex)errSecBadReq, NULL));
            
            require_action_quiet(CFDictionaryGetValueIfPresent(atLeastCharactersRef, kSecPasswordCharacterCount, &thresholdRef) != false, fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("The dictionary parameter 'At Least N Characters' is either null or not a string"),  (CFIndex)errSecBadReq, NULL));
            
            require_action_quiet(isNull(thresholdRef) != true, fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("The dictionary parameter 'characters' is either null or not a number"),  (CFIndex)errSecBadReq, NULL));
            require_action_quiet(isNumber(thresholdRef), fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("The dictionary parameter 'characters' is either null or not a number"), (CFIndex)errSecBadReq, NULL));
            
            require_action_quiet(CFDictionaryGetValueIfPresent(atLeastCharactersRef, kSecPasswordCharacters, &characters) != false, fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("The dictionary parameter 'At Least N Characters' is either null or not a string"),  (CFIndex)errSecBadReq, NULL));
            require_action_quiet(isNull(characters) != true, fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("The dictionary parameter 'Characters' is either null or not a string"),  (CFIndex)errSecBadReq, NULL));
            require_action_quiet(isString(characters), fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("The dictionary parameter 'Characters' is either null or not a string"), (CFIndex)errSecBadReq, NULL));
        }
       
        if(CFDictionaryGetValueIfPresent(passwordRequirements, kSecPasswordContainsNoMoreThanNConsecutiveIdenticalCharacters, &identicalRef)){
            require_action_quiet(isNull(identicalRef) != true, fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("The dictionary parameter 'Identical Consecutive Characters' is either null or not a number"),  (CFIndex)errSecBadReq, NULL));
            require_action_quiet(isNumber(identicalRef), fail, tempError = CFErrorCreate(kCFAllocatorDefault, CFSTR("The dictionary parameter 'Identical Consecutive Characters' is either null or not a number"), (CFIndex)errSecBadReq, NULL));
        }
    }
fail:
    if (tempError != NULL) {
        *error = tempError;
        CFRetain(*error);
        return false;
    }
    
    CFReleaseNull(tempError);
    return true;
}

static bool doesFinalPasswordPass(CFStringRef password, CFDictionaryRef requirements){
    
    CFTypeRef characters, identicalRef = NULL, NRef = NULL, endWith= NULL, startWith= NULL, atLeastCharacters= NULL, atMostCharacters = NULL;
    uint64_t valuePtr;
    CFIndex N, identicalCount;
    CFArrayRef requiredCharacterSet = (CFArrayRef)CFDictionaryGetValue(requirements, kSecRequiredCharacterSetsKey);
    
    if(!CFDictionaryGetValueIfPresent(requirements, kSecPasswordCantEndWithChars, &endWith))
        endWith = NULL;
    
    if(!CFDictionaryGetValueIfPresent(requirements, kSecPasswordCantStartWithChars, &startWith))
        startWith = NULL;
    
    if(!CFDictionaryGetValueIfPresent(requirements, kSecPasswordContainsAtLeastNSpecificCharacters, &atLeastCharacters))
        atLeastCharacters = NULL;
    
    if(!CFDictionaryGetValueIfPresent(requirements, kSecPasswordContainsNoMoreThanNSpecificCharacters, &atMostCharacters))
        atMostCharacters = NULL;
    
    if(!CFDictionaryGetValueIfPresent(requirements, kSecPasswordContainsNoMoreThanNConsecutiveIdenticalCharacters, &identicalRef))
        identicalRef = NULL;
    else{
        CFNumberGetValue((CFNumberRef)identicalRef, kCFNumberSInt64Type, &valuePtr);
        identicalCount = (long)valuePtr;
    }
    if(endWith != NULL)
    {
        if(!doesPasswordEndWith(password, endWith))
            return false;
    }
    if(startWith != NULL){
        if(!doesPasswordStartWith(password, startWith))
            return false;
    }
    if(atLeastCharacters != NULL){
        NRef = CFDictionaryGetValue(atLeastCharacters, kSecPasswordCharacterCount);
        characters = CFDictionaryGetValue(atLeastCharacters, kSecPasswordCharacters);
        CFNumberGetValue((CFNumberRef)NRef, kCFNumberSInt64Type, &valuePtr);
        N = (long)valuePtr;
        if(!passwordContainsAtLeastNCharacters(password, characters, N))
            return false;
    }
    if(atMostCharacters != NULL){
        NRef = CFDictionaryGetValue(atMostCharacters, kSecPasswordCharacterCount);
        characters = CFDictionaryGetValue(atMostCharacters, kSecPasswordCharacters);
        CFNumberGetValue((CFNumberRef)NRef, kCFNumberSInt64Type, &valuePtr);
        N = (long)valuePtr;
        if(!passwordContainsLessThanNCharacters(password, characters, N))
            return false;
    }
    if(identicalRef != NULL){
        if(!passwordContainsLessThanNIdenticalCharacters(password, identicalCount))
            return false;
    }
    if (!passwordContainsRequiredCharacters(password, requiredCharacterSet))
        return false;
    
    if(true == SecPasswordIsPasswordWeak(password))
        return false;
    
    return true;
}

//entry point into password generation
CF_RETURNS_RETAINED CFStringRef SecPasswordGenerate(SecPasswordType type, CFErrorRef *error, CFDictionaryRef passwordRequirements){
    bool check = false;
    CFTypeRef separator = NULL, defaults = NULL, groupSizeRef = NULL, numberOfGroupsRef = NULL;
    CFDictionaryRef properlyFormattedRequirements = NULL;
    *error = NULL;
    uint64_t valuePtr, groupSize, numberOfGroups;
    CFNumberRef numberOfRequiredRandomCharacters;
    CFIndex requiredCharactersSize;
    CFStringRef randomCharacters = NULL, password = NULL, allowedChars = NULL;
    CFMutableStringRef finalPassword = NULL;
    
    check = isDictionaryFormattedProperly(type, passwordRequirements, error);
    require_quiet(check != false, fail);
    
    //should we generate defaults?
    if(passwordRequirements == NULL || (CFDictionaryGetValueIfPresent(passwordRequirements, kSecPasswordDefaultForType, &defaults) && isString(defaults) == true && 0 == CFStringCompare(defaults, CFSTR("true"), 0) ))
        passwordGenerateDefaultParametersDictionary(&properlyFormattedRequirements, type, passwordRequirements);
    else
        passwordGenerationParametersDictionary(&properlyFormattedRequirements, type, passwordRequirements);
    
    CFRetain(properlyFormattedRequirements);
    
    require_quiet(*error == NULL && properlyFormattedRequirements != NULL, fail);
    
    numberOfRequiredRandomCharacters = (CFNumberRef)CFDictionaryGetValue(properlyFormattedRequirements, kSecNumberOfRequiredRandomCharactersKey);
    CFNumberGetValue(numberOfRequiredRandomCharacters, kCFNumberSInt64Type, &valuePtr);
    requiredCharactersSize = (long)valuePtr;
    
    if(!CFDictionaryGetValueIfPresent(properlyFormattedRequirements, kSecPasswordGroupSize, &groupSizeRef)){
        groupSizeRef = NULL;
    }
    else
        CFNumberGetValue((CFNumberRef)groupSizeRef, kCFNumberSInt64Type, &groupSize);
    
    if(!CFDictionaryGetValueIfPresent(properlyFormattedRequirements, kSecPasswordNumberOfGroups, &numberOfGroupsRef)){
        numberOfGroupsRef = NULL;
    }
    else
        CFNumberGetValue((CFNumberRef)numberOfGroupsRef, kCFNumberSInt64Type, &numberOfGroups);
    
    while (true) {
        allowedChars = CFDictionaryGetValue(properlyFormattedRequirements, kSecAllowedCharactersKey);
        getPasswordRandomCharacters(&randomCharacters, properlyFormattedRequirements, &requiredCharactersSize, allowedChars);
        
        if(numberOfGroupsRef && groupSizeRef){
            finalPassword = CFStringCreateMutable(kCFAllocatorDefault, 0);
            
            if(!CFDictionaryGetValueIfPresent(properlyFormattedRequirements, kSecPasswordSeparator, &separator))
                separator = NULL;
            
            if(separator == NULL)
                separator = CFSTR("-");
            
            CFIndex i = 0;
            while( i != requiredCharactersSize){
                if((i + (CFIndex)groupSize) < requiredCharactersSize){
                    CFStringAppend(finalPassword, CFStringCreateWithSubstring(kCFAllocatorDefault, randomCharacters, CFRangeMake(i, (CFIndex)groupSize)));
                    CFStringAppend(finalPassword, separator);
                    i+=groupSize;
                }
                else if((i+(CFIndex)groupSize) == requiredCharactersSize){
                    CFStringAppend(finalPassword, CFStringCreateWithSubstring(kCFAllocatorDefault, randomCharacters, CFRangeMake(i, (CFIndex)groupSize)));
                    i+=groupSize;
                }
                else {
                    CFStringAppend(finalPassword, CFStringCreateWithSubstring(kCFAllocatorDefault, randomCharacters, CFRangeMake(i, requiredCharactersSize - i)));
                    i+=(requiredCharactersSize - i);
                }
            }
            password = CFStringCreateCopy(kCFAllocatorDefault, finalPassword);
            CFReleaseNull(finalPassword);
        }
        //no fancy formatting
        else {
            password = CFStringCreateCopy(kCFAllocatorDefault, randomCharacters);
        }
        
        CFReleaseNull(randomCharacters);
        require_quiet(doesFinalPasswordPass(password, properlyFormattedRequirements), no_pass);
        return password;
        
    no_pass:
        CFReleaseNull(password);
    }
    
fail:
    CFReleaseNull(properlyFormattedRequirements);
    return NULL;
}

const char *in_word_set (const char *str, unsigned int len){
    static const char * wordlist[] =
    {
        "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "",
        "", "", "0103", "", "", "", "", "0123", "", "", "", "", "0303", "", "", "",
        "", "", "", "", "0110", "", "1103", "", "", "", "", "1123", "", "", "0000",
        "", "1203", "", "0404", "", "", "", "", "1234", "1110", "2015", "2013", "",
        "2014", "1010", "2005", "2003", "", "2004", "1210", "0505", "0111", "", "",
        "", "2008", "0101", "", "2007", "", "", "", "", "2006", "2010", "1995", "1993",
        "", "1994", "2000", "", "1111", "", "", "", "1998", "1101", "", "1997", "",
        "0808", "1211", "", "1996", "0102", "", "1201", "", "", "1990", "", "", "",
        "", "0202", "", "2011", "", "", "1112", "1958", "2001", "", "1957", "1102",
        "", "3333", "", "1956", "1212", "1985", "1983", "", "1984", "1202", "", "0909",
        "", "0606", "", "1988", "1991", "", "1987", "2012", "", "", "", "1986", "2002",
        "", "", "", "0707", "1980", "", "2009", "", "", "2222", "1965", "1963", "",
        "1964", "", "", "2229", "", "", "1992", "1968", "", "", "1967", "", "", "1999",
        "", "1966", "", "1975", "1973", "", "1974", "1960", "", "1981", "", "4444",
        "", "1978", "", "7465", "1977", "", "", "", "", "1976", "2580", "", "1959",
        "", "", "1970", "", "", "", "", "", "", "", "", "", "1982", "", "1961", "",
        "", "5252", "", "1989", "", "", "", "", "", "", "", "", "", "", "", "", "",
        "", "1971", "", "", "", "", "", "", "", "1962", "", "5683", "", "6666", "",
        "", "1969", "", "", "", "", "", "", "", "", "", "", "", "", "1972", "", "",
        "", "", "", "", "1979", "", "", "", "7667"
    };
    
    if (len <= MAX_WORD_LENGTH && len >= MIN_WORD_LENGTH)
    {
        register int key = pinhash (str, len);
        
        if (key <= MAX_HASH_VALUE && key >= 0)
        {
            register const char *s = wordlist[key];
            if (*str == *s && !strcmp (str + 1, s + 1))
                return s;
        }
    }
    return 0;
}
CFDictionaryRef SecPasswordCopyDefaultPasswordLength(SecPasswordType type, CFErrorRef *error){
    
    CFIndex tupleLengthInt, numOfTuplesInt;
    CFNumberRef tupleLength = NULL;
    CFNumberRef numOfTuples = NULL;
    
    CFMutableDictionaryRef passwordLengthDefaults = NULL;

    switch(type){
        case(kSecPasswordTypeiCloudRecovery):
            tupleLengthInt = 4;
            numOfTuplesInt = 6;
            tupleLength = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &tupleLengthInt);
            numOfTuples = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &numOfTuplesInt);
            passwordLengthDefaults = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, 0, 0);
            CFDictionaryAddValue(passwordLengthDefaults, kSecPasswordGroupSize, tupleLength);
            CFDictionaryAddValue(passwordLengthDefaults, kSecPasswordNumberOfGroups, numOfTuples);
            return CFDictionaryCreateCopy(kCFAllocatorDefault, passwordLengthDefaults);

        case(kSecPasswordTypePIN):
            tupleLengthInt = 4;
            numOfTuplesInt = 1;
            tupleLength = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &tupleLengthInt);
            numOfTuples = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &numOfTuplesInt);
            passwordLengthDefaults = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, 0, 0);
            CFDictionaryAddValue(passwordLengthDefaults, kSecPasswordGroupSize, tupleLength);
            CFDictionaryAddValue(passwordLengthDefaults, kSecPasswordNumberOfGroups, numOfTuples);
            return CFDictionaryCreateCopy(kCFAllocatorDefault, passwordLengthDefaults);
        
        case(kSecPasswordTypeSafari):
            tupleLengthInt = 4;
            numOfTuplesInt = 5;
            tupleLength = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &tupleLengthInt);
            numOfTuples = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &numOfTuplesInt);
            passwordLengthDefaults = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, 0, 0);
            CFDictionaryAddValue(passwordLengthDefaults, kSecPasswordGroupSize, tupleLength);
            CFDictionaryAddValue(passwordLengthDefaults, kSecPasswordNumberOfGroups, numOfTuples);
            return CFDictionaryCreateCopy(kCFAllocatorDefault, passwordLengthDefaults);

        
        case(kSecPasswordTypeWifi):
            tupleLengthInt = 4;
            numOfTuplesInt = 3;
            tupleLength = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &tupleLengthInt);
            numOfTuples = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &numOfTuplesInt);
            passwordLengthDefaults = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, 0, 0);
            CFDictionaryAddValue(passwordLengthDefaults, kSecPasswordGroupSize, tupleLength);
            CFDictionaryAddValue(passwordLengthDefaults, kSecPasswordNumberOfGroups, numOfTuples);
            return CFDictionaryCreateCopy(kCFAllocatorDefault, passwordLengthDefaults);

        
        default:
            if(SecError(errSecBadReq, error, CFSTR("Password type does not exist.")) == false)
            {
                secdebug("secpasswordcopydefaultpasswordlength", "could not create error!");
            }
            return NULL;
    }
}
